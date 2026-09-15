#pragma once

// The transport seam — a declared byte stream, a declared datagram endpoint, and the OS calls that
// carry them.
//
// This header names no OS type. <sys/socket.h> and <winsock2.h> appear only in socket_posix.cpp and
// socket_win32.cpp, so a translation unit that merely touches the seam pays for neither, and this file
// reads the same on every platform. The handle below is an integer the OS assigned; what the value
// means belongs to those two files.
//
// Every socket the seam opens is non-blocking. A transfer that cannot proceed answers WouldBlock and
// returns, and a caller that wants to wait says so with waitReadable and then reads — one
// implementation serving both usages, rather than a second blocking path beside the first.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace retropp::net {

// How far a call got. Every OS code the platform can tell apart maps onto one of these before it
// crosses the seam, so a caller reads no errno and sees no WSA constant. The OS code stays beside
// Other inside the implementation, where a diagnosis can still reach it.
enum class Status : std::uint8_t {
    Ok,
    WouldBlock,   // nothing to read, or the send window is full — the call returned instead of waiting
    Closed,       // the peer is gone, and stays gone
    Refused,      // the address is reachable and nothing is listening on it
    Unreachable,  // no route to the address
    TimedOut,
    Other,
    // A datagram arrived larger than the buffer offered it. What fit was copied and the rest is gone —
    // a datagram is delivered once, and the remainder cannot be asked for again. Appended rather than
    // placed among its neighbours so no existing enumerator's value moves.
    Truncated,
};

// How far a transfer got, and how many bytes moved. A short count on a stream is ordinary — the rest
// stays for the next call.
struct Transfer {
    Status      status = Status::Ok;
    std::size_t bytes  = 0;
};

// An address as a consumer writes one: a host and a port. The host may be a name, a v4 literal or a v6
// literal, and resolution happens where the address is used — so both families arrive without a caller
// choosing between them.
struct SocketAddress {
    std::string   host;
    std::uint16_t port = 0;
};

// The value no OS hands out, so a handle that names nothing says so.
inline constexpr std::uint64_t kNoSocket = ~std::uint64_t{0};

// A live socket. The width carries both a POSIX descriptor and a Windows SOCKET.
struct SocketHandle {
    std::uint64_t value = kNoSocket;

    [[nodiscard]] bool valid() const noexcept { return value != kNoSocket; }
};

// ── The OS calls ────────────────────────────────────────────────────────────────
// One declaration per capability, implemented twice; exactly one implementation compiles. Address
// resolution picks the family, so a v4 literal, a v6 literal and a name that resolves to either all
// travel through the same call.

// Connect a stream to an address, waiting up to the timeout for the handshake to settle. Connecting is
// the one call with a round-trip inside it, so the bound is a parameter rather than a constant chosen
// here: a caller that must not stall startup passes a short one, and TimedOut is a real answer.
[[nodiscard]] Status connectStream(const SocketAddress& to, std::chrono::milliseconds timeout,
                                   SocketHandle& out);

// Bind a stream socket and begin accepting on it. Port 0 asks the OS for an unused port; localPort
// reads back the one it chose.
[[nodiscard]] Status listenStream(const SocketAddress& at, int backlog, SocketHandle& out);

// Take the next connection off a listening socket. WouldBlock means none is waiting yet.
[[nodiscard]] Status acceptStream(SocketHandle listener, SocketHandle& out);

// Bind a datagram socket. Port 0 asks the OS for an unused port.
[[nodiscard]] Status bindDatagram(const SocketAddress& at, SocketHandle& out);

// The port the OS bound, which is what a caller that asked for 0 needs in order to be dialled.
[[nodiscard]] Status localPort(SocketHandle socket, std::uint16_t& out);

[[nodiscard]] Transfer sendStream(SocketHandle socket, std::span<const std::byte> bytes);
[[nodiscard]] Transfer receiveStream(SocketHandle socket, std::span<std::byte> into);

// One datagram per call in both directions: a send is one datagram, and a receive fills from exactly
// one and reports who sent it. Boundaries are preserved rather than coalesced, which is the difference
// that makes a datagram its own shape here instead of a setting on a stream.
[[nodiscard]] Transfer sendDatagram(SocketHandle socket, const SocketAddress& to,
                                    std::span<const std::byte> bytes);
[[nodiscard]] Transfer receiveDatagram(SocketHandle socket, std::span<std::byte> into,
                                       SocketAddress& from);

// Wait until the socket has something to read, or until the timeout expires. TimedOut is the ordinary
// answer for a quiet socket, not a failure.
[[nodiscard]] Status waitReadable(SocketHandle socket, std::chrono::milliseconds timeout);

void closeSocket(SocketHandle& socket) noexcept;

// Owns a handle and closes it once. Moving one hands the handle over; the source is left naming
// nothing, so a close happens exactly where the ownership ended up.
class OwnedSocket {
public:
    OwnedSocket() = default;
    explicit OwnedSocket(SocketHandle socket) noexcept : socket_(socket) {}

    OwnedSocket(const OwnedSocket&)            = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;

    OwnedSocket(OwnedSocket&& other) noexcept : socket_(std::exchange(other.socket_, SocketHandle{})) {}

    OwnedSocket& operator=(OwnedSocket&& other) noexcept {
        if (this != &other) {
            closeSocket(socket_);
            socket_ = std::exchange(other.socket_, SocketHandle{});
        }
        return *this;
    }

    ~OwnedSocket() { closeSocket(socket_); }

    [[nodiscard]] SocketHandle get() const noexcept { return socket_; }
    [[nodiscard]] bool         valid() const noexcept { return socket_.valid(); }

private:
    SocketHandle socket_{};
};

// ── The declared stream ─────────────────────────────────────────────────────────
// A byte stream between two parties, stated without naming a socket — which is what lets a transport
// with no OS underneath it satisfy the same contract and stand in wherever a socket stream stands.
//
// A closed peer is a state to observe rather than an event delivered: closed() keeps answering true,
// and a read after the peer is gone reports Closed again instead of turning into an error.
class Stream {
public:
    Stream()          = default;
    virtual ~Stream() = default;

    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] virtual Transfer send(std::span<const std::byte> bytes)              = 0;
    [[nodiscard]] virtual Transfer receive(std::span<std::byte> into)                  = 0;
    [[nodiscard]] virtual Status   waitReadable(std::chrono::milliseconds timeout)     = 0;
    [[nodiscard]] virtual bool     closed() const noexcept                            = 0;
};

// A stream carried by an OS socket.
class SocketStream final : public Stream {
public:
    explicit SocketStream(SocketHandle socket) noexcept : socket_(socket) {}
    explicit SocketStream(OwnedSocket socket) noexcept : socket_(std::move(socket)) {}

    [[nodiscard]] Transfer send(std::span<const std::byte> bytes) override {
        const Transfer t = sendStream(socket_.get(), bytes);
        if (t.status == Status::Closed) closed_ = true;
        return t;
    }

    [[nodiscard]] Transfer receive(std::span<std::byte> into) override {
        if (closed_) return Transfer{.status = Status::Closed, .bytes = 0};
        const Transfer t = receiveStream(socket_.get(), into);
        if (t.status == Status::Closed) closed_ = true;
        return t;
    }

    [[nodiscard]] Status waitReadable(std::chrono::milliseconds timeout) override {
        return net::waitReadable(socket_.get(), timeout);
    }

    [[nodiscard]] bool closed() const noexcept override { return closed_; }

    [[nodiscard]] SocketHandle handle() const noexcept { return socket_.get(); }

private:
    OwnedSocket socket_{};
    bool        closed_ = false;
};

}  // namespace retropp::net
