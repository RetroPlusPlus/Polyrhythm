// The transport seam: a stream, a datagram endpoint, and an in-process pair asserting the same claims
// the socket path does.
//
// Every listener binds port 0 and reads back the port the OS chose. Five runners take jobs at the same
// time, so a fixed port number is a flake waiting for the least convenient moment.

#include "src/net/in_process.h"
#include "src/net/socket.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using retropp::net::acceptStream;
using retropp::net::bindDatagram;
using retropp::net::closeSocket;
using retropp::net::connectStream;
using retropp::net::InProcessStream;
using retropp::net::listenStream;
using retropp::net::localPort;
using retropp::net::openInProcessStream;
using retropp::net::receiveDatagram;
using retropp::net::sendDatagram;
using retropp::net::SocketAddress;
using retropp::net::SocketHandle;
using retropp::net::SocketStream;
using retropp::net::Status;
using retropp::net::Transfer;
using retropp::net::waitReadable;

constexpr auto kSettle = 2000ms;  // generous: a loopback handshake is immediate, a loaded runner is not

std::vector<std::byte> bytesOf(std::initializer_list<int> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const int v : values) bytes.push_back(static_cast<std::byte>(v));
    return bytes;
}

// Brings up a listener on an ephemeral port of the given host, dials it, and hands back the two
// connected ends. Both are the caller's to own.
testing::AssertionResult connectedPair(const std::string& host, SocketHandle& client,
                                       SocketHandle& server) {
    SocketHandle listener;
    if (const Status s = listenStream({.host = host, .port = 0}, 4, listener); s != Status::Ok) {
        return testing::AssertionFailure() << "listen on " << host << " answered " << int(s);
    }

    std::uint16_t port = 0;
    if (const Status s = localPort(listener, port); s != Status::Ok || port == 0) {
        closeSocket(listener);
        return testing::AssertionFailure() << "the listener reports no bound port";
    }

    if (const Status s = connectStream({.host = host, .port = port}, kSettle, client);
        s != Status::Ok) {
        closeSocket(listener);
        return testing::AssertionFailure() << "connect to " << host << ":" << port << " answered "
                                           << int(s);
    }

    // The connection is queued on the listener the moment the handshake lands; waiting for readability
    // is what makes the accept deterministic rather than a race against the loopback.
    if (const Status s = waitReadable(listener, kSettle); s != Status::Ok) {
        closeSocket(listener);
        closeSocket(client);
        return testing::AssertionFailure() << "no connection arrived at the listener";
    }
    if (const Status s = acceptStream(listener, server); s != Status::Ok) {
        closeSocket(listener);
        closeSocket(client);
        return testing::AssertionFailure() << "accept answered " << int(s);
    }

    closeSocket(listener);
    return testing::AssertionSuccess();
}

// A port with nothing behind it: bind one, read which one the OS chose, then give it back.
std::uint16_t unusedPort(const std::string& host) {
    SocketHandle listener;
    if (listenStream({.host = host, .port = 0}, 1, listener) != Status::Ok) return 0;
    std::uint16_t port = 0;
    if (localPort(listener, port) != Status::Ok) port = 0;
    closeSocket(listener);
    return port;
}

// Reads until the expected count arrives, so a stream delivered in pieces is assembled rather than
// asserted against one call's return.
std::vector<std::byte> readExactly(SocketStream& from, std::size_t count, std::size_t chunk) {
    std::vector<std::byte> assembled;
    while (assembled.size() < count) {
        if (waitReadable(from.handle(), kSettle) != Status::Ok) break;
        std::vector<std::byte> piece(chunk);
        const Transfer         t = from.receive(piece);
        if (t.status != Status::Ok) break;
        assembled.insert(assembled.end(), piece.begin(), piece.begin() + t.bytes);
    }
    return assembled;
}

TEST(NetSocket, StreamRoundTripsOnLoopback) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("127.0.0.1", clientSide, serverSide));

    SocketStream client{clientSide};
    SocketStream server{serverSide};

    const std::vector<std::byte> sent = bytesOf({0x29, 0x55, 0x01});
    const Transfer               put  = client.send(sent);
    ASSERT_EQ(put.status, Status::Ok);
    ASSERT_EQ(put.bytes, sent.size());

    const std::vector<std::byte> got = readExactly(server, sent.size(), sent.size());
    EXPECT_EQ(got, sent);
}

TEST(NetSocket, StreamPreservesOrderAcrossPartialReads) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("127.0.0.1", clientSide, serverSide));

    SocketStream client{clientSide};
    SocketStream server{serverSide};

    // A sequence that reads differently backwards, so reassembling it in reverse is visible.
    const std::vector<std::byte> sent = bytesOf({1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_EQ(client.send(sent).status, Status::Ok);

    // Three at a time: the reader takes several passes to see eight bytes.
    const std::vector<std::byte> got = readExactly(server, sent.size(), 3);
    EXPECT_EQ(got, sent);
}

TEST(NetSocket, ClosedIsAStableStateNotAnEvent) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("127.0.0.1", clientSide, serverSide));

    SocketStream client{clientSide};
    { SocketStream leaving{serverSide}; }  // the peer goes away

    ASSERT_EQ(waitReadable(client.handle(), kSettle), Status::Ok);

    std::array<std::byte, 4> into{};
    const Transfer           first = client.receive(into);
    EXPECT_EQ(first.status, Status::Closed);
    EXPECT_EQ(first.bytes, 0u);
    EXPECT_TRUE(client.closed());

    // Asking again reports the same state rather than turning into an error.
    const Transfer second = client.receive(into);
    EXPECT_EQ(second.status, Status::Closed);
    EXPECT_TRUE(client.closed());
}

TEST(NetSocket, DatagramBoundariesArePreserved) {
    SocketHandle sender;
    SocketHandle receiver;
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, sender), Status::Ok);
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, receiver), Status::Ok);

    std::uint16_t receiverPort = 0;
    ASSERT_EQ(localPort(receiver, receiverPort), Status::Ok);
    const SocketAddress to{.host = "127.0.0.1", .port = receiverPort};

    const std::vector<std::byte> shortOne = bytesOf({0x11, 0x22});
    const std::vector<std::byte> longOne  = bytesOf({0x31, 0x32, 0x33, 0x34, 0x35});
    ASSERT_EQ(sendDatagram(sender, to, shortOne).status, Status::Ok);
    ASSERT_EQ(sendDatagram(sender, to, longOne).status, Status::Ok);

    // A buffer large enough to hold both, so coalescing would show up as one longer read.
    std::array<std::byte, 64> into{};
    SocketAddress             from;

    ASSERT_EQ(waitReadable(receiver, kSettle), Status::Ok);
    const Transfer first = receiveDatagram(receiver, into, from);
    ASSERT_EQ(first.status, Status::Ok);
    EXPECT_EQ(first.bytes, shortOne.size());

    ASSERT_EQ(waitReadable(receiver, kSettle), Status::Ok);
    const Transfer second = receiveDatagram(receiver, into, from);
    ASSERT_EQ(second.status, Status::Ok);
    EXPECT_EQ(second.bytes, longOne.size());

    closeSocket(sender);
    closeSocket(receiver);
}

TEST(NetSocket, DatagramCarriesItsSenderAddress) {
    SocketHandle sender;
    SocketHandle receiver;
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, sender), Status::Ok);
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, receiver), Status::Ok);

    std::uint16_t senderPort   = 0;
    std::uint16_t receiverPort = 0;
    ASSERT_EQ(localPort(sender, senderPort), Status::Ok);
    ASSERT_EQ(localPort(receiver, receiverPort), Status::Ok);

    const std::vector<std::byte> payload = bytesOf({0x7E});
    ASSERT_EQ(sendDatagram(sender, {.host = "127.0.0.1", .port = receiverPort}, payload).status,
              Status::Ok);

    std::array<std::byte, 16> into{};
    SocketAddress             from;
    ASSERT_EQ(waitReadable(receiver, kSettle), Status::Ok);
    ASSERT_EQ(receiveDatagram(receiver, into, from).status, Status::Ok);

    EXPECT_EQ(from.port, senderPort);
    EXPECT_EQ(from.host, "127.0.0.1");

    closeSocket(sender);
    closeSocket(receiver);
}

TEST(NetSocket, ADatagramTooLargeForItsBufferSaysSo) {
    SocketHandle sender;
    SocketHandle receiver;
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, sender), Status::Ok);
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, receiver), Status::Ok);

    std::uint16_t senderPort   = 0;
    std::uint16_t receiverPort = 0;
    ASSERT_EQ(localPort(sender, senderPort), Status::Ok);
    ASSERT_EQ(localPort(receiver, receiverPort), Status::Ok);

    // Forty bytes offered a buffer of eight. A caller reading only the count could not tell this from an
    // eight-byte datagram, and the thirty-two that did not fit are gone — a datagram is delivered once.
    std::vector<std::byte> large(40);
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<std::byte>(i + 1);
    ASSERT_EQ(sendDatagram(sender, {.host = "127.0.0.1", .port = receiverPort}, large).status,
              Status::Ok);

    std::array<std::byte, 8> into{};
    SocketAddress            from;
    ASSERT_EQ(waitReadable(receiver, kSettle), Status::Ok);
    const Transfer got = receiveDatagram(receiver, into, from);

    EXPECT_EQ(got.status, Status::Truncated);
    EXPECT_EQ(got.bytes, into.size());  // what fit, never what was sent
    EXPECT_EQ(from.port, senderPort);   // and the sender is still named

    // What did fit is the datagram's beginning, in order.
    EXPECT_EQ(into.front(), static_cast<std::byte>(1));
    EXPECT_EQ(into.back(), static_cast<std::byte>(8));

    closeSocket(sender);
    closeSocket(receiver);
}

TEST(NetSocket, ReceiveOnAnEmptySocketWouldBlockRatherThanFail) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("127.0.0.1", clientSide, serverSide));

    SocketStream client{clientSide};
    SocketStream server{serverSide};

    std::array<std::byte, 8> into{};
    const Transfer           t = server.receive(into);
    EXPECT_EQ(t.status, Status::WouldBlock);
    EXPECT_EQ(t.bytes, 0u);
    // A quiet socket is not a closed one.
    EXPECT_FALSE(server.closed());
}

TEST(NetSocket, ConnectToAClosedPortIsRefused) {
    const std::uint16_t port = unusedPort("127.0.0.1");
    ASSERT_NE(port, 0);

    // A longer bound than the other cases take, because a refusal does not arrive equally fast on every
    // platform. The claim is unchanged — the error maps to refused rather than to the catch-all — and
    // only the patience differs.
    constexpr auto kRefusal = 8000ms;

    SocketHandle client;
    const Status s = connectStream({.host = "127.0.0.1", .port = port}, kRefusal, client);
    EXPECT_EQ(s, Status::Refused);
    closeSocket(client);
}

TEST(NetSocket, ReadinessWaitHonoursItsTimeout) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("127.0.0.1", clientSide, serverSide));

    SocketStream client{clientSide};
    SocketStream server{serverSide};

    constexpr auto requested = 100ms;
    const auto     began     = std::chrono::steady_clock::now();
    const Status   s         = waitReadable(server.handle(), requested);
    const auto     waited    = std::chrono::steady_clock::now() - began;

    EXPECT_EQ(s, Status::TimedOut);
    // It waited rather than returning at once, and it came back rather than waiting forever. The bounds
    // are wide on purpose: the exact instant belongs to the host's scheduler.
    EXPECT_GE(waited, requested / 2);
    EXPECT_LT(waited, 30 * requested);
}

TEST(NetSocket, StreamRoundTripsOverIpv6Loopback) {
    SocketHandle clientSide;
    SocketHandle serverSide;
    ASSERT_TRUE(connectedPair("::1", clientSide, serverSide));

    SocketStream client{clientSide};
    SocketStream server{serverSide};

    const std::vector<std::byte> sent = bytesOf({0xA0, 0xB1, 0xC2});
    ASSERT_EQ(client.send(sent).status, Status::Ok);

    const std::vector<std::byte> got = readExactly(server, sent.size(), sent.size());
    EXPECT_EQ(got, sent);
}

TEST(NetInProcess, InProcessStreamRoundTripsLikeALoopbackStream) {
    InProcessStream first;
    InProcessStream second;
    ASSERT_EQ(openInProcessStream("round trip", first), Status::Ok);
    ASSERT_EQ(openInProcessStream("round trip", second), Status::Ok);

    const std::vector<std::byte> outbound = bytesOf({1, 2, 3, 4});
    ASSERT_EQ(first.send(outbound).status, Status::Ok);

    ASSERT_EQ(second.waitReadable(kSettle), Status::Ok);
    std::vector<std::byte> into(outbound.size());
    const Transfer         got = second.receive(into);
    ASSERT_EQ(got.status, Status::Ok);
    ASSERT_EQ(got.bytes, outbound.size());
    EXPECT_EQ(into, outbound);

    // The same claim in the other direction: both ends carry a stream, not one reader and one writer.
    const std::vector<std::byte> back = bytesOf({9, 8, 7});
    ASSERT_EQ(second.send(back).status, Status::Ok);
    ASSERT_EQ(first.waitReadable(kSettle), Status::Ok);
    std::vector<std::byte> heard(back.size());
    const Transfer         returned = first.receive(heard);
    ASSERT_EQ(returned.status, Status::Ok);
    EXPECT_EQ(heard, back);
}

TEST(NetInProcess, InProcessPairsByName) {
    InProcessStream alphaFirst;
    InProcessStream alphaSecond;
    InProcessStream beta;
    ASSERT_EQ(openInProcessStream("alpha", alphaFirst), Status::Ok);
    ASSERT_EQ(openInProcessStream("alpha", alphaSecond), Status::Ok);
    ASSERT_EQ(openInProcessStream("beta", beta), Status::Ok);

    const std::vector<std::byte> payload = bytesOf({0x5A, 0x5B});
    ASSERT_EQ(alphaFirst.send(payload).status, Status::Ok);

    // The end that named the same pair hears it.
    std::vector<std::byte> into(payload.size());
    ASSERT_EQ(alphaSecond.waitReadable(kSettle), Status::Ok);
    const Transfer heard = alphaSecond.receive(into);
    ASSERT_EQ(heard.status, Status::Ok);
    EXPECT_EQ(into, payload);

    // The end that named a different pair hears nothing, and waiting on it times out rather than
    // delivering someone else's bytes.
    std::vector<std::byte> elsewhere(payload.size());
    EXPECT_EQ(beta.receive(elsewhere).status, Status::WouldBlock);
    EXPECT_EQ(beta.waitReadable(50ms), Status::TimedOut);

    // A pair has two ends, and a third is turned away.
    InProcessStream third;
    EXPECT_EQ(openInProcessStream("alpha", third), Status::Refused);
    EXPECT_FALSE(third.open());
}

TEST(NetInProcess, InProcessReportsClosedLikeASocket) {
    InProcessStream held;
    ASSERT_EQ(openInProcessStream("closing", held), Status::Ok);
    {
        InProcessStream leaving;
        ASSERT_EQ(openInProcessStream("closing", leaving), Status::Ok);
    }

    std::array<std::byte, 4> into{};
    const Transfer           first = held.receive(into);
    EXPECT_EQ(first.status, Status::Closed);
    EXPECT_EQ(first.bytes, 0u);
    EXPECT_TRUE(held.closed());

    const Transfer second = held.receive(into);
    EXPECT_EQ(second.status, Status::Closed);
    EXPECT_TRUE(held.closed());
}

}  // namespace
