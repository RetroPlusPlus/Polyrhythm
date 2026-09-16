// TLS over the transport seam: a handshake, a byte round-trip, and a refusal — and the same three
// through the in-process pair, with no socket underneath any of it.
//
// Client and server are both ours, entirely on loopback, which is what makes the refusal assertable at
// all: asserting that an untrusted certificate is rejected needs something to present one.
//
// Every case registers on every platform. Where a platform's TLS stack has not landed yet the body is
// replaced by a visible skip naming what lifts it, so the counts stay identical across machines and the
// gap is legible in CI output rather than absent from it.

#include "src/net/in_process.h"
#include "src/net/socket.h"
#if defined(RETROPP_TLS_READY)
#include "src/net/tls.h"
#endif

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

#if !defined(RETROPP_TLS_READY)
constexpr const char* kPendingStack =
    "this platform's TLS stack is not built yet — OpenSSL lands in the Linux session, SChannel in the "
    "Windows session";
#endif

#if defined(RETROPP_TLS_READY)

using retropp::net::acceptStream;
using retropp::net::certificateExpiry;
using retropp::net::Certificate;
using retropp::net::closeSocket;
using retropp::net::connectStream;
using retropp::net::InProcessStream;
using retropp::net::listenStream;
using retropp::net::localPort;
using retropp::net::openInProcessStream;
using retropp::net::openTlsClient;
using retropp::net::openTlsServer;
using retropp::net::SocketHandle;
using retropp::net::SocketStream;
using retropp::net::Status;
using retropp::net::Stream;
using retropp::net::TlsClientConfig;
using retropp::net::TlsServerConfig;
using retropp::net::TlsStream;
using retropp::net::Transfer;

constexpr auto kSettle   = 5000ms;  // generous: a loopback handshake is immediate, a loaded runner is not
constexpr auto kPassword = "retropp-test";

// How long the handshake driver keeps going before calling it stuck. Deliberately far longer than any
// handshake takes — nothing here asserts how FAST a handshake is, and this bound exists only so a stall
// ends the case instead of the run. Set wide because the alternative is a bound that encodes how loaded
// the machine was: a handshake that settles in a fifth of a second locally has been seen to miss five
// seconds once inside a larger batch, and a liveness guard tripping on machine load is a flake, not a
// finding.
constexpr auto kLiveness = 60000ms;

std::vector<std::byte> readFixture(const std::string& name) {
    const std::string path = std::string(RETROPP_FIXTURES_DIR) + "/tls/" + name;
    std::ifstream     file(path, std::ios::binary);
    if (!file) return {};

    // Named rather than written inline: as a temporary the second iterator parses as a function type.
    std::istreambuf_iterator<char> begin(file);
    std::istreambuf_iterator<char> end;
    const std::vector<char>        raw(begin, end);

    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

// A connected pair of loopback sockets, each end owned by the caller.
testing::AssertionResult socketPair(std::unique_ptr<Stream>& first, std::unique_ptr<Stream>& second) {
    SocketHandle listener;
    if (listenStream({.host = "127.0.0.1", .port = 0}, 4, listener) != Status::Ok) {
        return testing::AssertionFailure() << "could not listen on loopback";
    }
    std::uint16_t port = 0;
    if (localPort(listener, port) != Status::Ok || port == 0) {
        closeSocket(listener);
        return testing::AssertionFailure() << "the listener reports no bound port";
    }

    SocketHandle dialled;
    if (connectStream({.host = "127.0.0.1", .port = port}, kSettle, dialled) != Status::Ok) {
        closeSocket(listener);
        return testing::AssertionFailure() << "could not connect to the listener";
    }
    if (retropp::net::waitReadable(listener, kSettle) != Status::Ok) {
        closeSocket(listener);
        closeSocket(dialled);
        return testing::AssertionFailure() << "no connection arrived at the listener";
    }
    SocketHandle accepted;
    if (acceptStream(listener, accepted) != Status::Ok) {
        closeSocket(listener);
        closeSocket(dialled);
        return testing::AssertionFailure() << "could not accept";
    }
    closeSocket(listener);

    first  = std::make_unique<SocketStream>(dialled);
    second = std::make_unique<SocketStream>(accepted);
    return testing::AssertionSuccess();
}

// Two ends of a named in-process pair — the same contract with no OS beneath it.
testing::AssertionResult inProcessPair(const char* name, std::unique_ptr<Stream>& first,
                                       std::unique_ptr<Stream>& second) {
    auto one = std::make_unique<InProcessStream>();
    auto two = std::make_unique<InProcessStream>();
    if (openInProcessStream(name, *one) != Status::Ok ||
        openInProcessStream(name, *two) != Status::Ok) {
        return testing::AssertionFailure() << "could not open both ends of '" << name << "'";
    }
    first  = std::move(one);
    second = std::move(two);
    return testing::AssertionSuccess();
}

// Carries both ends to a conclusion. Two ends living in one thread each need the other's bytes, so
// neither may wait: they alternate with a zero timeout, making what progress they can.
//
// Answers Ok once both have settled, Untrusted the moment the client reaches that verdict, and TimedOut
// if neither happens in time.
Status driveHandshake(TlsStream& client, TlsStream& server) {
    const auto deadline = std::chrono::steady_clock::now() + kLiveness;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.settled()) {
            const Status step = client.handshake(0ms);
            if (step != Status::Ok && step != Status::WouldBlock) return step;
        }
        if (!server.settled()) {
            const Status step = server.handshake(0ms);
            if (step != Status::Ok && step != Status::WouldBlock) return step;
        }
        if (client.settled() && server.settled()) return Status::Ok;
    }
    return Status::TimedOut;
}

// Reads until the expected count arrives, so a payload delivered across several records is assembled
// rather than asserted against one call's return.
std::vector<std::byte> readExactly(TlsStream& from, std::size_t count, std::size_t chunk) {
    std::vector<std::byte> assembled;
    const auto             deadline = std::chrono::steady_clock::now() + kLiveness;
    while (assembled.size() < count && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::byte> piece(chunk);
        const Transfer         moved = from.receive(piece);
        if (moved.status == Status::Ok) {
            assembled.insert(assembled.end(), piece.begin(), piece.begin() + moved.bytes);
            continue;
        }
        if (moved.status == Status::WouldBlock) {
            if (from.waitReadable(100ms) == Status::Closed) break;
            continue;
        }
        break;
    }
    return assembled;
}

// A sequence that reads differently backwards, so reassembling it in reverse is visible.
std::vector<std::byte> risingPattern(std::size_t length) {
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::byte>((i * 7 + 1) & 0xFF);
    return bytes;
}

// A client that accepts exactly the loopback certificate and nothing else, and a server presenting the
// identity named.
struct Ends {
    TlsStream client;
    TlsStream server;
};

testing::AssertionResult bringUp(std::unique_ptr<Stream> clientCarrier,
                                 std::unique_ptr<Stream> serverCarrier, const char* serverIdentity,
                                 Ends& ends) {
    const std::vector<std::byte> identity = readFixture(serverIdentity);
    const std::vector<std::byte> pinned   = readFixture("loopback-cert.der");
    if (identity.empty() || pinned.empty()) {
        return testing::AssertionFailure() << "a TLS fixture is missing or empty";
    }

    const TlsClientConfig accepting{.hostname = "localhost",
                                    .pinned   = std::vector<Certificate>{pinned}};
    const TlsServerConfig presenting{.identity = identity, .password = kPassword};

    if (openTlsClient(std::move(clientCarrier), accepting, ends.client) != Status::Ok) {
        return testing::AssertionFailure() << "could not take the client side";
    }
    if (openTlsServer(std::move(serverCarrier), presenting, ends.server) != Status::Ok) {
        return testing::AssertionFailure() << "could not take the server side";
    }
    return testing::AssertionSuccess();
}

#endif  // RETROPP_TLS_READY

TEST(NetTls, HandshakeCompletesOverLoopback) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));

    EXPECT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);
    EXPECT_TRUE(ends.client.settled());
    EXPECT_TRUE(ends.server.settled());
#endif
}

TEST(NetTls, BytesRoundTripThroughTls) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));
    ASSERT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);

    const std::vector<std::byte> sent = risingPattern(64);
    ASSERT_EQ(ends.client.send(sent).status, Status::Ok);

    const std::vector<std::byte> got = readExactly(ends.server, sent.size(), sent.size());
    EXPECT_EQ(got, sent);
#endif
}

TEST(NetTls, AnUntrustedCertificateIsRefused) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    // The server presents a certificate the client does not pin. Nothing else differs.
    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "other.p12", ends));

    const Status verdict = driveHandshake(ends.client, ends.server);
    EXPECT_EQ(verdict, Status::Untrusted);
    EXPECT_NE(verdict, Status::Other);  // a refusal, never the catch-all
    EXPECT_FALSE(ends.client.settled());
#endif
}

TEST(NetTls, TlsCarriesTheInProcessTransport) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(inProcessPair("tls in process", clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));
    ASSERT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);

    // The whole encrypted path, with no socket anywhere beneath it.
    const std::vector<std::byte> sent = risingPattern(48);
    ASSERT_EQ(ends.server.send(sent).status, Status::Ok);

    const std::vector<std::byte> got = readExactly(ends.client, sent.size(), sent.size());
    EXPECT_EQ(got, sent);
#endif
}

TEST(NetTls, HandshakeResumesAfterWouldBlock) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));

    // Driven alone, the client cannot finish: the server owes it bytes nobody has asked the server to
    // send. That is a pause, not a failure.
    const Status alone = ends.client.handshake(0ms);
    EXPECT_EQ(alone, Status::WouldBlock);
    EXPECT_FALSE(ends.client.settled());

    // Resuming with both ends driven settles it, from exactly where it stopped.
    EXPECT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);
    EXPECT_TRUE(ends.client.settled());
#endif
}

TEST(NetTls, PartialRecordsReassembleThroughTls) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));
    ASSERT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);

    // Enough to cross more than one record, read back in small pieces: what arrives is the payload in
    // order, never a record boundary showing through.
    const std::vector<std::byte> sent = risingPattern(4000);
    std::size_t                  put  = 0;
    while (put < sent.size()) {
        const Transfer moved = ends.client.send(
            std::span<const std::byte>{sent.data() + put, sent.size() - put});
        ASSERT_TRUE(moved.status == Status::Ok || moved.status == Status::WouldBlock);
        put += moved.bytes;
    }

    const std::vector<std::byte> got = readExactly(ends.server, sent.size(), 100);
    EXPECT_EQ(got, sent);
#endif
}

TEST(NetTls, ClosedIsAStableStateThroughTls) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    std::unique_ptr<Stream> clientCarrier;
    std::unique_ptr<Stream> serverCarrier;
    ASSERT_TRUE(socketPair(clientCarrier, serverCarrier));

    Ends ends;
    ASSERT_TRUE(bringUp(std::move(clientCarrier), std::move(serverCarrier), "loopback.p12", ends));
    ASSERT_EQ(driveHandshake(ends.client, ends.server), Status::Ok);

    { TlsStream leaving = std::move(ends.server); }  // the peer goes away

    std::array<std::byte, 16> into{};
    const Transfer            first = ends.client.receive(into);
    EXPECT_EQ(first.status, Status::Closed);

    // Asking again reports the same state rather than turning into an error.
    const Transfer second = ends.client.receive(into);
    EXPECT_EQ(second.status, Status::Closed);
#endif
}

TEST(NetTls, TheFixtureCertificateHasLifeLeft) {
#if !defined(RETROPP_TLS_READY)
    GTEST_SKIP() << kPendingStack;
#else
    const std::vector<std::byte> pinned = readFixture("loopback-cert.der");
    ASSERT_FALSE(pinned.empty());

    std::int64_t expires = 0;
    ASSERT_EQ(certificateExpiry(pinned, expires), Status::Ok);

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Fails while there is still a year to spare, so an expiring fixture announces itself as itself
    // rather than surfacing years later as an inscrutable handshake failure on all five machines.
    constexpr std::int64_t kOneYear = 365LL * 24 * 60 * 60;
    EXPECT_GT(expires, now + kOneYear)
        << "the TLS test certificate expires within a year — regenerate tests/fixtures/tls/";
#endif
}

}  // namespace
