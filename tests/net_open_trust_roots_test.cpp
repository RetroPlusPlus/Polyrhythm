// Opening a TLS stream in one call against the operating system's own trust roots: a public host is
// trusted under the name its row dials, and a certificate issued for another name is refused.
//
// Both cases reach real hosts on the public internet, so they run outside the regression sweep, in the
// TLS step every CI job carries: an outage then shows as that step's failure, with the status it met.
// Nothing here skips. An open that answers TimedOut — its connection or its handshake ran past kOpen — is
// the network's answer, and the case opens again, up to kAttempts in all, printing each retry. Every other
// answer, a verdict included, is judged as it first comes back. A refusal is asserted as Untrusted
// exactly, so a dead network never passes for a refused certificate.
//
// Every case runs on every platform, against whichever stack that platform's TLS is built on.

#include "retropp/net.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

namespace {

using namespace std::chrono_literals;

namespace net = retropp::net;

constexpr auto kOpen     = 10000ms;
constexpr auto kLiveness = 60000ms;
constexpr int  kAttempts = 3;  // how many times a case opens a host whose open answers TimedOut

const char* name(net::Status s) {
    switch (s) {
        case net::Status::Ok:          return "Ok";
        case net::Status::WouldBlock:  return "WouldBlock";
        case net::Status::Closed:      return "Closed";
        case net::Status::Refused:     return "Refused";
        case net::Status::Unreachable: return "Unreachable";
        case net::Status::TimedOut:    return "TimedOut";
        case net::Status::Other:       return "Other";
        case net::Status::Truncated:   return "Truncated";
        case net::Status::Untrusted:   return "Untrusted";
    }
    return "?";
}

// Opens `host` on 443 over TLS with nothing pinned and no name in the config, up to kAttempts times while
// the open answers TimedOut. Any other answer ends it at once.
net::Opened openTrusting(const char* host) {
    for (int attempt = 1;; ++attempt) {
        net::Opened opened =
            net::open(net::Tcp{.host = host, .port = 443, .timeout = kOpen}, net::TlsClientConfig{});
        if (opened.status != net::Status::TimedOut || attempt == kAttempts) return opened;
        std::cout << host << ": attempt " << attempt << " of " << kAttempts
                  << " — open answered TimedOut; opening again" << std::endl;
    }
}

TEST(NetTlsTrustRootsOpen, OpenTrustsAPublicHostUnderTheRowsName) {
    net::Opened opened = openTrusting("github.com");
    ASSERT_EQ(opened.status, net::Status::Ok) << "github.com: open answered " << name(opened.status);
    ASSERT_NE(opened.stream, nullptr);

    const std::string_view request = "HEAD / HTTP/1.1\r\nHost: github.com\r\nConnection: close\r\n\r\n";
    const auto             bytes   = std::as_bytes(std::span(request.data(), request.size()));
    std::size_t            sent    = 0;
    const auto             deadline = std::chrono::steady_clock::now() + kLiveness;
    while (sent < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const net::Transfer t = opened.stream->send(bytes.subspan(sent));
        if (t.status != net::Status::Ok && t.status != net::Status::WouldBlock) {
            FAIL() << "send answered " << name(t.status);
        }
        sent += t.bytes;
    }
    ASSERT_EQ(sent, bytes.size());

    std::array<std::byte, 64> reply{};
    std::size_t               got = 0;
    while (got < 9 && std::chrono::steady_clock::now() < deadline) {
        const net::Transfer t = opened.stream->receive(std::span(reply).subspan(got));
        got += t.bytes;
        if (t.status == net::Status::WouldBlock) {
            (void)opened.stream->waitReadable(1000ms);
        } else if (t.status != net::Status::Ok) {
            break;
        }
    }
    ASSERT_GE(got, 9u) << "no reply came back through the encrypted stream";
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(reply.data()), 9), "HTTP/1.1 ");
}

TEST(NetTlsTrustRootsOpen, OpenRefusesACertificateForAnotherName) {
    // The config names no host, so the name checked is the row's.
    const net::Opened opened = openTrusting("wrong.host.badssl.com");
    EXPECT_EQ(opened.status, net::Status::Untrusted)
        << "wrong.host.badssl.com: open answered " << name(opened.status);
    EXPECT_EQ(opened.stream, nullptr);
}

}  // namespace
