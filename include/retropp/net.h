#pragma once

// Networking — a byte stream between two parties, a datagram socket, TLS over any stream, and the address
// helpers a game composes them with.
//
// Every call answers at once. A transfer that cannot proceed answers WouldBlock and returns, and a caller
// that wants to wait says so with waitReadable and then reads. The calls with a round-trip inside them —
// opening a stream and a TLS handshake — take the longest they may wait as a value.
//
// A stream is the same four calls whether a socket, the in-process pair or TLS carries it, so a protocol
// written against Stream runs over any of them unchanged. This header names no OS type.
//
// A game links this with the platform and configures nothing. A binary carries the transport when its code
// calls it, and the OS's TLS library when its code names TLS.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace retropp::net {

// How far a call got. Every OS code the platform can tell apart maps onto one of these, so a caller reads
// no errno and sees no WSA constant.
enum class Status : std::uint8_t {
    Ok,
    WouldBlock,   // nothing to read, or the send window is full — the call returned rather than wait
    Closed,       // the peer is gone, and stays gone
    Refused,      // the address is reachable and nothing is listening on it
    Unreachable,  // no route to the address
    TimedOut,
    Other,
    // A datagram arrived larger than the buffer offered it. What fit was copied and the rest is gone —
    // a datagram is delivered once, and the remainder cannot be asked for again.
    Truncated,
    // The peer's certificate is not one this side accepts. A verdict rather than a stall: it does not
    // change on a later call.
    Untrusted,
};

// How far a transfer got, and how many bytes moved. A short count on a stream is ordinary — the rest
// stays for the next call.
struct Transfer {
    Status      status = Status::Ok;
    std::size_t bytes  = 0;
};

// An address as a game writes one: a host and a port. The host is a name, a v4 literal or a v6 literal,
// and it is resolved where the address is used. An empty host is every interface's IPv4 address
// (0.0.0.0) when listening or binding — "::" names the IPv6 one — and the loopback when dialing.
struct Endpoint {
    std::string   host{};
    std::uint16_t port = 0;
};

// ── The stream ──────────────────────────────────────────────────────────────────
// A byte stream between two parties, stated without naming a socket, so a transport with no OS underneath
// it satisfies the same contract and stands wherever a socket stream stands.
//
// A closed peer is a state to observe rather than an event delivered: closed() keeps answering true, and
// a read after the peer is gone answers Closed again.
//
// One thread uses a stream at a time. A stream is held through a std::unique_ptr and may be handed to
// another thread whole.
class Stream {
public:
    Stream()          = default;
    virtual ~Stream() = default;

    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] virtual Transfer send(std::span<const std::byte> bytes)              = 0;
    [[nodiscard]] virtual Transfer receive(std::span<std::byte> into)                  = 0;
    // Wait until there is something to read, or until the timeout expires. TimedOut is the ordinary
    // answer for a quiet stream. A closed peer counts as readable: the read that follows answers Closed.
    [[nodiscard]] virtual Status   waitReadable(std::chrono::milliseconds timeout)     = 0;
    [[nodiscard]] virtual bool     closed() const noexcept                            = 0;
};

// ── TLS ─────────────────────────────────────────────────────────────────────────
// TLS is a byte transformer: it takes a Stream and is a Stream. It runs over a socket and equally over
// the in-process pair, so an encrypted protocol is exercised with no OS socket anywhere. The library is
// the OS's own — SecureTransport, SChannel, the system's OpenSSL.
//
// A client always validates what its peer presents: it is handed the certificates it accepts, or it
// defers to the OS's trust roots. A certificate no public authority signed is accepted by pinning it.

// One certificate, DER-encoded.
using Certificate = std::vector<std::byte>;

// What a client accepts from the server it dials.
struct TlsClientConfig {
    // The name the peer's certificate must carry.
    std::string hostname{};
    // Certificates accepted on their own authority. Empty means the OS's trust roots decide; a non-empty
    // list means the peer must present one of these.
    std::vector<Certificate> pinned{};
};

// What a server presents to clients that dial it.
struct TlsServerConfig {
    // A PKCS#12 container holding the certificate and its private key.
    std::vector<std::byte> identity{};
    std::string            password{};
};

struct Secured;

// A stream whose bytes are encrypted in transit. It owns the stream it runs over. Bring one up with
// tlsClient or tlsServer, drive handshake until it settles, then read and write it like any other stream.
class TlsStream final : public Stream {
public:
    // The stack's own handshake state, defined by the implementation.
    struct State;

    TlsStream();
    ~TlsStream() override;

    TlsStream(TlsStream&& other) noexcept;
    TlsStream& operator=(TlsStream&& other) noexcept;

    // Carry the handshake as far as the bytes available allow. Ok once settled, and again on every later
    // call. WouldBlock while the peer still owes bytes: call it again. Untrusted when the peer's
    // certificate is not one this side accepts.
    //
    // A zero timeout makes what progress it can and returns, which is how two ends in one thread are
    // driven: alternate between them until both settle. A longer timeout waits that long for the peer's
    // next bytes before answering WouldBlock.
    [[nodiscard]] Status handshake(std::chrono::milliseconds timeout);

    // True once the handshake has completed. A send or a receive before then answers WouldBlock, having
    // moved nothing.
    [[nodiscard]] bool settled() const noexcept;

    [[nodiscard]] Transfer send(std::span<const std::byte> bytes) override;
    [[nodiscard]] Transfer receive(std::span<std::byte> into) override;
    [[nodiscard]] Status   waitReadable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool     closed() const noexcept override;

private:
    friend Secured tlsClient(std::unique_ptr<Stream>, const TlsClientConfig&);
    friend Secured tlsServer(std::unique_ptr<Stream>, const TlsServerConfig&);

    std::unique_ptr<State> state_;
};

// What tlsClient and tlsServer answer: how the call went, and the stream when it went Ok. The handshake
// has not run.
struct Secured {
    Status                     status = Status::Other;
    std::unique_ptr<TlsStream> stream{};
};

// Take the client side of a TLS connection over `over`, accepting what `accepting` describes. Closed,
// with no stream, when `over` holds nothing.
[[nodiscard]] Secured tlsClient(std::unique_ptr<Stream> over, const TlsClientConfig& accepting);

// Take the server side over `over`, presenting `presenting`'s identity to whoever dialed in. Closed, with
// no stream, when `over` holds nothing.
[[nodiscard]] Secured tlsServer(std::unique_ptr<Stream> over, const TlsServerConfig& presenting);

// ── Opening a stream ────────────────────────────────────────────────────────────
// A row names the transport and where it leads; open takes the row and answers a stream. The code that
// uses the stream is the same under every row.

// A TCP connection to a host and port.
struct Tcp {
    std::string   host{};
    std::uint16_t port = 0;
    // The longest one wait may last: the connection attempt to each address the host resolves to, and
    // the TLS handshake when open is given a TlsClientConfig. Resolving a name is the OS's own call and
    // is outside this bound.
    std::chrono::milliseconds timeout{5000};
};

// One end of a named pair inside this process. Two ends that name the same pair are joined; a third is
// refused, because a pair has two ends. An end whose name nobody else opens is a working stream with
// nobody on the far side: its reads answer WouldBlock, the answer a quiet socket gives.
struct InProcess {
    std::string pair{};
};

// What open and accept answer: how the call went, and the stream when it went Ok. The stream is null
// under every other status.
struct Opened {
    Status                  status = Status::Other;
    std::unique_ptr<Stream> stream{};
};

// Dial a TCP stream. Refused when nothing listens at the address, Unreachable when no route leads to it,
// TimedOut when an attempt outlasts the row's timeout.
[[nodiscard]] Opened open(const Tcp& to);

// Take one end of an in-process pair. Refused when both ends of the name are taken.
[[nodiscard]] Opened open(const InProcess& at);

// Dial a TCP stream and bring TLS up over it; the stream answered has finished its handshake. An empty
// `accepting.hostname` takes the row's host. Untrusted when the peer's certificate is not one `accepting`
// accepts.
[[nodiscard]] Opened open(const Tcp& to, const TlsClientConfig& accepting);

// ── Listening ───────────────────────────────────────────────────────────────────

struct Listening;

// A bound TCP port taking connections. It owns the port: moving one hands the port over and leaves the
// source holding nothing, and destroying it stops the listening. A listener holding nothing answers
// Closed.
class Listener {
public:
    Listener() noexcept = default;
    ~Listener();

    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    // Take the next connection waiting. WouldBlock when none is.
    [[nodiscard]] Opened accept();

    // Wait until a connection is waiting, or until the timeout expires. TimedOut is the ordinary answer
    // for a quiet port.
    [[nodiscard]] Status waitReadable(std::chrono::milliseconds timeout);

    // The port bound — the one the OS chose when the endpoint asked for port 0.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    friend Listening listen(const Endpoint& at, int backlog);

    std::uint64_t handle_ = ~std::uint64_t{0};
    std::uint16_t port_   = 0;
};

// What listen answers: how the call went, and the listener when it went Ok.
struct Listening {
    Status   status = Status::Other;
    Listener listener{};
};

// Bind a TCP port and begin taking connections on it. Port 0 asks the OS for an unused port. `backlog`
// is how many connections may wait to be accepted.
[[nodiscard]] Listening listen(const Endpoint& at, int backlog = 16);

// ── Datagrams ───────────────────────────────────────────────────────────────────

// What a datagram receive answers: how the call went, how many bytes were copied, and who sent them.
struct Received {
    Status      status = Status::Other;
    std::size_t bytes  = 0;
    Endpoint    from{};
};

struct Bound;

// A bound UDP port. One datagram per call in both directions: a send is one datagram, and a receive
// fills from exactly one and names its sender. It may send to a broadcast address. It owns the port the
// way a Listener does, and one holding nothing answers Closed.
class DatagramSocket {
public:
    DatagramSocket() noexcept = default;
    ~DatagramSocket();

    DatagramSocket(DatagramSocket&& other) noexcept;
    DatagramSocket& operator=(DatagramSocket&& other) noexcept;

    DatagramSocket(const DatagramSocket&)            = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    [[nodiscard]] Transfer send(const Endpoint& to, std::span<const std::byte> bytes);

    // WouldBlock when no datagram is waiting. Truncated when the datagram is larger than `into`: what
    // fit was copied, and the rest is gone.
    [[nodiscard]] Received receive(std::span<std::byte> into);

    [[nodiscard]] Status waitReadable(std::chrono::milliseconds timeout);

    // The port bound — the one the OS chose when the endpoint asked for port 0.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    friend Bound bindDatagram(const Endpoint& at);

    std::uint64_t handle_ = ~std::uint64_t{0};
    std::uint16_t port_   = 0;
};

// What bindDatagram answers: how the call went, and the socket when it went Ok.
struct Bound {
    Status         status = Status::Other;
    DatagramSocket socket{};
};

// Bind a UDP port. Port 0 asks the OS for an unused port.
[[nodiscard]] Bound bindDatagram(const Endpoint& at);

// ── Addresses ───────────────────────────────────────────────────────────────────

// Read "host:port" into an Endpoint: "192.168.1.42:7654", "example.com:443", "[::1]:7654", and ":7654"
// for an empty host. A v6 literal is written in brackets. The text is read as written and no name is
// looked up. Answers nothing when the text has no port, the port is not a number from 0 to 65535, or a
// v6 literal is outside brackets.
[[nodiscard]] std::optional<Endpoint> parseEndpoint(std::string_view text);

// One address this machine holds on one of its network interfaces.
struct Interface {
    std::string name{};            // the OS's name for the interface: "en0", "eth0", "Ethernet"
    std::string address{};         // a v4 or v6 literal
    int         prefixLength = 0;  // how many leading bits name the network: 24 for 255.255.255.0
    bool        loopback     = false;  // the address reaches this machine only
};

// Every address on every interface that is up, loopback included, in the order the OS lists them. The
// addresses another machine dials are the ones with `loopback` false.
[[nodiscard]] std::vector<Interface> localAddresses();

// The address that reaches every machine on an interface's network: its address with every bit past the
// prefix set. 192.168.1.42 with a 24-bit prefix answers "192.168.1.255". Answers nothing for a v6
// address, which has no broadcast.
[[nodiscard]] std::optional<std::string> broadcastAddress(const Interface& of);

// ── Retrying ────────────────────────────────────────────────────────────────────

// How the wait between attempts grows.
struct Backoff {
    std::chrono::milliseconds base{250};   // the wait after the first attempt
    std::chrono::milliseconds max{30000};  // the longest wait
    // How far a wait may fall either side of its length, as a fraction of it.
    float jitter = 0.2f;
    // What the jitter is drawn from; callers that seed differently wait differently.
    std::uint32_t seed = 0;
};

// How long to wait after attempt number `attempt` (0 for the first) before trying again: `base` doubled
// once per attempt, held at `max`, then moved by the jitter. The same arguments answer the same wait, and
// the count of attempts is the caller's to keep.
[[nodiscard]] std::chrono::milliseconds backoffDelay(int attempt, const Backoff& policy = {});

}  // namespace retropp::net
