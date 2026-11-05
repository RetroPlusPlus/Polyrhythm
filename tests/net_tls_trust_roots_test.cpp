// TLS against the operating system's own trust roots: a client that pins nothing trusts a public server
// through the system's certificate store, and refuses the certificates a public client must refuse — expired,
// issued for another name, self-signed, and chained to a root the system does not hold.
//
// These cases dial real hosts on the public internet, so they run outside the regression sweep, as their own
// step in every CI job: a network outage then shows as that step's failure, with the status it met, instead
// of a red sweep. Nothing here skips. A connection that does not open fails with the status named, and a
// refusal is asserted as Untrusted exactly — a dead network never passes for a refused certificate.
//
// Every case runs on every platform, against whichever stack that platform's TLS is built on.

#include "src/net/socket.h"
#include "src/net/tls.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

using retropp::net::connectStream;
using retropp::net::openTlsClient;
using retropp::net::SocketAddress;
using retropp::net::SocketHandle;
using retropp::net::SocketStream;
using retropp::net::Status;
using retropp::net::TlsClientConfig;
using retropp::net::TlsStream;
using retropp::net::Transfer;

constexpr auto kConnect = 10000ms;
// How long the handshake driver keeps going before calling it stuck. Far longer than a handshake over the
// internet takes: nothing here asserts how fast one is, and the bound exists only so a stall ends the case.
constexpr auto kLiveness = 60000ms;

const char* name(Status s) {
    switch (s) {
        case Status::Ok:          return "Ok";
        case Status::WouldBlock:  return "WouldBlock";
        case Status::Closed:      return "Closed";
        case Status::Refused:     return "Refused";
        case Status::Unreachable: return "Unreachable";
        case Status::TimedOut:    return "TimedOut";
        case Status::Other:       return "Other";
        case Status::Truncated:   return "Truncated";
        case Status::Untrusted:   return "Untrusted";
    }
    return "?";
}

// Dial `host` on 443 and take the client side with nothing pinned, so the system's trust roots decide. The
// connection itself must open; what the handshake answers is the case's to judge.
struct Dialed {
    Status    connected = Status::Other;
    TlsStream tls;
};

Dialed dial(const std::string& host) {
    Dialed d;
    SocketHandle socket{};
    d.connected = connectStream(SocketAddress{.host = host, .port = 443}, kConnect, socket);
    if (d.connected != Status::Ok) return d;
    const Status opened = openTlsClient(std::make_unique<SocketStream>(socket),
                                        TlsClientConfig{.hostname = host, .pinned = {}}, d.tls);
    if (opened != Status::Ok) d.connected = opened;
    return d;
}

// Drive the handshake until it answers anything but WouldBlock, or the liveness bound runs out.
Status settle(TlsStream& tls) {
    const auto deadline = std::chrono::steady_clock::now() + kLiveness;
    for (;;) {
        const Status s = tls.handshake(1000ms);
        if (s != Status::WouldBlock || std::chrono::steady_clock::now() >= deadline) return s;
    }
}

void expectRefused(const std::string& host) {
    Dialed d = dial(host);
    ASSERT_EQ(d.connected, Status::Ok) << host << ": the connection did not open (" << name(d.connected)
                                       << ") — the network, not the certificate";
    const Status verdict = settle(d.tls);
    EXPECT_EQ(verdict, Status::Untrusted) << host << ": the handshake answered " << name(verdict);
}

TEST(NetTlsTrustRoots, APublicHostIsTrustedThroughTheSystemRoots) {
    const std::string host = "github.com";
    Dialed d = dial(host);
    ASSERT_EQ(d.connected, Status::Ok) << host << ": the connection did not open (" << name(d.connected) << ")";
    const Status verdict = settle(d.tls);
    ASSERT_EQ(verdict, Status::Ok) << host << ": the handshake answered " << name(verdict);

    const std::string_view request = "HEAD / HTTP/1.1\r\nHost: github.com\r\nConnection: close\r\n\r\n";
    const auto bytes = std::as_bytes(std::span(request.data(), request.size()));
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + kLiveness;
    while (sent < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const Transfer t = d.tls.send(bytes.subspan(sent));
        if (t.status != Status::Ok && t.status != Status::WouldBlock) FAIL() << "send answered " << name(t.status);
        sent += t.bytes;
    }
    ASSERT_EQ(sent, bytes.size());

    std::array<std::byte, 64> reply{};
    std::size_t               got = 0;
    while (got < 9 && std::chrono::steady_clock::now() < deadline) {
        const Transfer t = d.tls.receive(std::span(reply).subspan(got));
        got += t.bytes;
        if (t.status == Status::WouldBlock) {
            (void)d.tls.waitReadable(1000ms);
        } else if (t.status != Status::Ok) {
            break;
        }
    }
    ASSERT_GE(got, 9u) << "no reply came back through the encrypted stream";
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(reply.data()), 9), "HTTP/1.1 ");
}

TEST(NetTlsTrustRoots, AnExpiredCertificateIsRefused) { expectRefused("expired.badssl.com"); }

TEST(NetTlsTrustRoots, ACertificateForAnotherNameIsRefused) { expectRefused("wrong.host.badssl.com"); }

TEST(NetTlsTrustRoots, ASelfSignedCertificateIsRefused) { expectRefused("self-signed.badssl.com"); }

TEST(NetTlsTrustRoots, AnUntrustedRootIsRefused) { expectRefused("untrusted-root.badssl.com"); }

}  // namespace
