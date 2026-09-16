#pragma once

// TLS over the transport seam — a client, a server, and the trust each side applies.
//
// TLS here is a byte transformer, not a transport of its own: it takes a `Stream` and is a `Stream`. So
// it composes over a socket and equally over the in-process pair, which is what lets the whole encrypted
// path be exercised with no OS socket anywhere.
//
// Each platform's stack — SecureTransport, OpenSSL, SChannel — appears only in its own `.cpp`, and the
// handshake state lives behind an incomplete type, so every OS type and all the macro weather that comes
// with those headers stays out of this file and out of anything that includes it.
//
// A client always validates what its peer presents: it is handed the certificates it will accept, or it
// defers to the OS's own trust roots. It cannot be told to trust anything, and per-connection trust is
// what makes that workable — a test needing a certificate no public CA signed pins that certificate,
// which is a better answer than a switch that turns checking off.

#include "src/net/socket.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace retropp::net {

// One certificate, DER-encoded.
using Certificate = std::vector<std::byte>;

// What a client accepts from the server it dials.
struct TlsClientConfig {
    // The name the peer's certificate must carry.
    std::string hostname;
    // Certificates accepted on their own authority. Empty means the OS's trust roots decide, which is
    // what a client talking to the public internet wants; a non-empty list means the peer must present
    // one of these and nothing else will do.
    std::vector<Certificate> pinned;
};

// What a server presents to clients that dial it.
struct TlsServerConfig {
    // A PKCS#12 container holding the certificate and its private key — the one container all three
    // platform stacks import.
    std::vector<std::byte> identity;
    std::string            password;
};

// A stream whose bytes are encrypted in transit.
//
// The carrier is owned from construction: a TLS connection is one thing to hand around rather than two
// that have to be kept together. Bring one up with openTlsClient / openTlsServer, drive `handshake`
// until it settles, then read and write it like any other stream.
class TlsStream final : public Stream {
public:
    // The stack's own handshake state. Opaque here — whichever implementation compiles defines it, which
    // is what keeps every TLS type out of this header and out of anything that includes it.
    struct State;

    // Declared here and defined alongside the destructor rather than defaulted inline: a defaulted
    // constructor would have to know how to destroy the state it holds, and the whole point of holding it
    // by an incomplete type is that this header does not.
    TlsStream();
    ~TlsStream() override;

    TlsStream(TlsStream&& other) noexcept;
    TlsStream& operator=(TlsStream&& other) noexcept;

    // Carry the handshake as far as the bytes available allow.
    //
    // `Ok` once settled — and calling it again after that is harmless. `WouldBlock` while the peer still
    // owes bytes, which is not a failure: call it again when there are more. `Untrusted` when the peer's
    // certificate is not one this side accepts, which is a verdict rather than a stall and does not
    // change on a later call.
    //
    // A zero timeout means "make what progress you can and return", which is how two ends living in one
    // thread are driven: alternate between them until both settle. A longer timeout waits that long for
    // the peer's next bytes before answering WouldBlock.
    [[nodiscard]] Status handshake(std::chrono::milliseconds timeout);

    // True once the handshake has completed. Sending or receiving before then is a caller error rather
    // than a hazard the stream guards: it answers WouldBlock, having moved nothing.
    [[nodiscard]] bool settled() const noexcept;

    [[nodiscard]] Transfer send(std::span<const std::byte> bytes) override;
    [[nodiscard]] Transfer receive(std::span<std::byte> into) override;
    [[nodiscard]] Status   waitReadable(std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool     closed() const noexcept override;

private:
    friend Status openTlsClient(std::unique_ptr<Stream>, const TlsClientConfig&, TlsStream&);
    friend Status openTlsServer(std::unique_ptr<Stream>, const TlsServerConfig&, TlsStream&);

    std::unique_ptr<State> state_;
};

// Take the client side of a TLS connection over `over`. The handshake has not run yet — drive it with
// TlsStream::handshake.
[[nodiscard]] Status openTlsClient(std::unique_ptr<Stream> over, const TlsClientConfig& accepting,
                                   TlsStream& out);

// Take the server side, presenting `presenting`'s identity to whoever dials in.
[[nodiscard]] Status openTlsServer(std::unique_ptr<Stream> over, const TlsServerConfig& presenting,
                                   TlsStream& out);

// When a certificate stops being valid, as a Unix timestamp. The platform stacks all answer this, and a
// test that asserts a fixture still has life left is the reason it is reachable: an expiry nobody planned
// for otherwise surfaces years later as an inscrutable handshake failure.
[[nodiscard]] Status certificateExpiry(std::span<const std::byte> der, std::int64_t& unixSeconds);

}  // namespace retropp::net
