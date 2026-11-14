// The networking surface as a game reaches it through <retropp/net.h>: a stream opened over TCP or the
// in-process pair, a listener that accepts, a datagram socket that names its sender, and TLS opened in
// one call or wrapped over any stream.
//
// Every listener and socket binds port 0 and reads back the port the OS chose, so jobs running side by
// side never meet on a fixed number. Every case runs on every platform.

#include "retropp/net.h"

// The public header brings no OS socket header with it.
#if defined(AF_INET) || defined(SOCK_STREAM) || defined(_WINSOCK2API_)
#error "retropp/net.h pulled an OS socket header into this translation unit"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace net = retropp::net;

constexpr auto kSettle   = 5000ms;
constexpr auto kPassword = "retropp-test";

// How long a loop that drives a handshake or assembles a read keeps going before it gives up. Nothing
// here asserts how fast either is; the bound ends a stalled case rather than the run.
constexpr auto kLiveness = 60000ms;

std::vector<std::byte> bytesOf(std::initializer_list<int> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const int v : values) bytes.push_back(static_cast<std::byte>(v));
    return bytes;
}

// A sequence that reads differently backward, so bytes delivered out of order are visible.
std::vector<std::byte> risingPattern(std::size_t length) {
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<std::byte>((i * 7 + 1) & 0xFF);
    return bytes;
}

std::vector<std::byte> readFixture(const std::string& name) {
    const std::string path = std::string(RETROPP_FIXTURES_DIR) + "/tls/" + name;
    std::ifstream     file(path, std::ios::binary);
    if (!file) return {};

    std::istreambuf_iterator<char> begin(file);
    std::istreambuf_iterator<char> end;
    const std::vector<char>        raw(begin, end);

    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

// Sends every byte, going round again while the send window is full.
bool sendAll(net::Stream& to, std::span<const std::byte> bytes) {
    std::size_t put      = 0;
    const auto  deadline = std::chrono::steady_clock::now() + kLiveness;
    while (put < bytes.size() && std::chrono::steady_clock::now() < deadline) {
        const net::Transfer moved = to.send(bytes.subspan(put));
        if (moved.status != net::Status::Ok && moved.status != net::Status::WouldBlock) return false;
        put += moved.bytes;
    }
    return put == bytes.size();
}

// Reads until `count` bytes have arrived, `chunk` at a time, so a stream delivered in pieces is
// assembled rather than asserted against one call's return.
std::vector<std::byte> readExactly(net::Stream& from, std::size_t count, std::size_t chunk) {
    std::vector<std::byte> assembled;
    const auto             deadline = std::chrono::steady_clock::now() + kLiveness;
    while (assembled.size() < count && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::byte> piece(chunk < count - assembled.size() ? chunk : count - assembled.size());
        const net::Transfer    moved = from.receive(piece);
        if (moved.status == net::Status::Ok) {
            assembled.insert(assembled.end(), piece.begin(),
                             piece.begin() + static_cast<std::ptrdiff_t>(moved.bytes));
            continue;
        }
        if (moved.status == net::Status::WouldBlock) {
            (void)from.waitReadable(100ms);
            continue;
        }
        break;
    }
    return assembled;
}

// A connected TCP pair on the loopback: the end that dialed and the end that was accepted.
testing::AssertionResult tcpPair(std::unique_ptr<net::Stream>& dialed,
                                 std::unique_ptr<net::Stream>& accepted) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    if (listening.status != net::Status::Ok) return testing::AssertionFailure() << "could not listen";

    net::Opened out = net::open(net::Tcp{.host = "127.0.0.1", .port = listening.listener.port()});
    if (out.status != net::Status::Ok) return testing::AssertionFailure() << "could not dial";
    if (listening.listener.waitReadable(kSettle) != net::Status::Ok) {
        return testing::AssertionFailure() << "no connection arrived at the listener";
    }
    net::Opened in = listening.listener.accept();
    if (in.status != net::Status::Ok) return testing::AssertionFailure() << "could not accept";

    dialed   = std::move(out.stream);
    accepted = std::move(in.stream);
    return testing::AssertionSuccess();
}

// Both ends of a named in-process pair.
testing::AssertionResult inProcessPair(const char* name, std::unique_ptr<net::Stream>& first,
                                       std::unique_ptr<net::Stream>& second) {
    net::Opened one = net::open(net::InProcess{.pair = name});
    net::Opened two = net::open(net::InProcess{.pair = name});
    if (one.status != net::Status::Ok || two.status != net::Status::Ok) {
        return testing::AssertionFailure() << "could not open both ends of '" << name << "'";
    }
    first  = std::move(one.stream);
    second = std::move(two.stream);
    return testing::AssertionSuccess();
}

// One exchange, written against Stream alone: 37 bytes sent in three writes and read back five at a
// time. It answers what arrived.
std::vector<std::byte> exchange(net::Stream& from, net::Stream& to) {
    const std::vector<std::byte> sent = risingPattern(37);
    const std::span<const std::byte> all{sent};
    if (!sendAll(from, all.subspan(0, 12)) || !sendAll(from, all.subspan(12, 12)) ||
        !sendAll(from, all.subspan(24))) {
        return {};
    }
    return readExactly(to, sent.size(), 5);
}

// Whether the ASCII bytes of `text` appear anywhere in `bytes`.
bool contains(const std::vector<std::byte>& bytes, std::string_view text) {
    const auto same = [](std::byte b, char c) {
        return b == static_cast<std::byte>(static_cast<unsigned char>(c));
    };
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end(), same) != bytes.end();
}

// Reads one stream until it answers Closed, waiting for the close to arrive.
net::Transfer readUntilClosed(net::Stream& from) {
    std::array<std::byte, 16> into{};
    net::Transfer             last{};
    const auto                deadline = std::chrono::steady_clock::now() + kLiveness;
    do {
        if (from.waitReadable(kSettle) == net::Status::TimedOut) break;
        last = from.receive(into);
    } while (last.status != net::Status::Closed && std::chrono::steady_clock::now() < deadline);
    return last;
}

// The server side of a TLS case, run on its own thread: accept one connection, take TLS over it with
// the identity named, drive the handshake, and — when `echo` is set and the handshake settled — read
// 48 bytes and send them back.
struct TlsServerRun {
    net::Status handshake = net::Status::Other;
    bool        echoed    = false;
};

void serveTls(net::Listener& listener, const std::vector<std::byte>& identity, bool echo,
              TlsServerRun& run) {
    if (listener.waitReadable(kSettle) != net::Status::Ok) return;
    net::Opened accepted = listener.accept();
    if (accepted.status != net::Status::Ok) return;

    net::Secured secured = net::tlsServer(std::move(accepted.stream),
                                          net::TlsServerConfig{.identity = identity, .password = kPassword});
    if (secured.status != net::Status::Ok) {
        run.handshake = secured.status;
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + kLiveness;
    net::Status step    = net::Status::WouldBlock;
    while (step == net::Status::WouldBlock && std::chrono::steady_clock::now() < deadline) {
        step = secured.stream->handshake(100ms);
    }
    run.handshake = step;
    if (!echo || step != net::Status::Ok) return;

    const std::vector<std::byte> heard = readExactly(*secured.stream, 48, 48);
    if (heard.size() != 48) return;
    run.echoed = sendAll(*secured.stream, heard);
    // Holds the stream open until the client has read the echo back.
    (void)readUntilClosed(*secured.stream);
}

TEST(NetConnection, AStreamOpensListensAndCarriesBytesBothWays) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);
    ASSERT_NE(listening.listener.port(), 0);

    net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = listening.listener.port()});
    ASSERT_EQ(dialed.status, net::Status::Ok);
    ASSERT_NE(dialed.stream, nullptr);

    ASSERT_EQ(listening.listener.waitReadable(kSettle), net::Status::Ok);
    net::Opened accepted = listening.listener.accept();
    ASSERT_EQ(accepted.status, net::Status::Ok);
    ASSERT_NE(accepted.stream, nullptr);

    const std::vector<std::byte> out = bytesOf({0x29, 0x55, 0x01});
    ASSERT_TRUE(sendAll(*dialed.stream, out));
    EXPECT_EQ(readExactly(*accepted.stream, out.size(), out.size()), out);

    const std::vector<std::byte> back = bytesOf({0x10, 0x20});
    ASSERT_TRUE(sendAll(*accepted.stream, back));
    EXPECT_EQ(readExactly(*dialed.stream, back.size(), back.size()), back);
}

TEST(NetConnection, AcceptWithNobodyWaitingWouldBlock) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);

    const net::Opened accepted = listening.listener.accept();
    EXPECT_EQ(accepted.status, net::Status::WouldBlock);
    EXPECT_EQ(accepted.stream, nullptr);
}

TEST(NetConnection, OpenToAPortNothingListensOnIsRefused) {
    std::uint16_t port = 0;
    {
        net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
        ASSERT_EQ(listening.status, net::Status::Ok);
        port = listening.listener.port();
    }
    ASSERT_NE(port, 0);

    // A refusal does not arrive equally fast on every platform, so the bound is wider than the others.
    const net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = port, .timeout = 8000ms});
    EXPECT_EQ(dialed.status, net::Status::Refused);
    EXPECT_EQ(dialed.stream, nullptr);
}

TEST(NetConnection, AnUnnamedHostListensWhereAV4DialReachesIt) {
    net::Listening listening = net::listen({.port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);

    const net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = listening.listener.port()});
    ASSERT_EQ(dialed.status, net::Status::Ok);

    ASSERT_EQ(listening.listener.waitReadable(kSettle), net::Status::Ok);
    EXPECT_EQ(listening.listener.accept().status, net::Status::Ok);
}

TEST(NetConnection, TheInProcessRowJoinsTwoEndsAndRefusesAThird) {
    net::Opened first  = net::open(net::InProcess{.pair = "net connection pair"});
    net::Opened second = net::open(net::InProcess{.pair = "net connection pair"});
    ASSERT_EQ(first.status, net::Status::Ok);
    ASSERT_EQ(second.status, net::Status::Ok);

    const net::Opened third = net::open(net::InProcess{.pair = "net connection pair"});
    EXPECT_EQ(third.status, net::Status::Refused);
    EXPECT_EQ(third.stream, nullptr);

    const std::vector<std::byte> sent = bytesOf({0x41, 0x42, 0x43, 0x44});
    ASSERT_TRUE(sendAll(*first.stream, sent));
    EXPECT_EQ(readExactly(*second.stream, sent.size(), sent.size()), sent);
}

TEST(NetConnection, OneExchangeRunsUnchangedOverEitherRow) {
    std::unique_ptr<net::Stream> dialed;
    std::unique_ptr<net::Stream> accepted;
    ASSERT_TRUE(tcpPair(dialed, accepted));

    std::unique_ptr<net::Stream> first;
    std::unique_ptr<net::Stream> second;
    ASSERT_TRUE(inProcessPair("net connection exchange", first, second));

    const std::vector<std::byte> overTcp       = exchange(*dialed, *accepted);
    const std::vector<std::byte> overInProcess = exchange(*first, *second);
    EXPECT_EQ(overTcp, risingPattern(37));
    EXPECT_EQ(overInProcess, risingPattern(37));
    EXPECT_EQ(overTcp, overInProcess);
}

TEST(NetConnection, AClosedPeerIsAStateOnEitherRow) {
    std::unique_ptr<net::Stream> dialed;
    std::unique_ptr<net::Stream> accepted;
    ASSERT_TRUE(tcpPair(dialed, accepted));

    std::unique_ptr<net::Stream> first;
    std::unique_ptr<net::Stream> second;
    ASSERT_TRUE(inProcessPair("net connection closing", first, second));

    const auto peerLeaves = [](std::unique_ptr<net::Stream>& held, std::unique_ptr<net::Stream>& leaving) {
        leaving.reset();

        EXPECT_EQ(readUntilClosed(*held).status, net::Status::Closed);
        EXPECT_TRUE(held->closed());

        // Asking again answers the same state.
        std::array<std::byte, 4> into{};
        EXPECT_EQ(held->receive(into).status, net::Status::Closed);
    };
    peerLeaves(dialed, accepted);
    peerLeaves(first, second);
}

TEST(NetConnection, ADatagramKeepsItsBoundaryAndNamesItsSender) {
    net::Bound sender   = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    net::Bound receiver = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(sender.status, net::Status::Ok);
    ASSERT_EQ(receiver.status, net::Status::Ok);

    const std::vector<std::byte> shortOne = bytesOf({0x11, 0x22, 0x33});
    const std::vector<std::byte> longOne  = bytesOf({0x51, 0x52, 0x53, 0x54, 0x55});

    const net::Endpoint to{.host = "127.0.0.1", .port = receiver.socket.port()};
    const net::Transfer first  = sender.socket.send(to, shortOne);
    const net::Transfer second = sender.socket.send(to, longOne);
    ASSERT_EQ(first.status, net::Status::Ok);
    ASSERT_EQ(second.status, net::Status::Ok);

    // A buffer large enough for both, so two datagrams joined would show as one longer read.
    for (const std::vector<std::byte>* expected : {&shortOne, &longOne}) {
        std::array<std::byte, 64> into{};
        ASSERT_EQ(receiver.socket.waitReadable(kSettle), net::Status::Ok);
        const net::Received heard = receiver.socket.receive(into);
        ASSERT_EQ(heard.status, net::Status::Ok);
        ASSERT_EQ(heard.bytes, expected->size());
        EXPECT_EQ(std::vector<std::byte>(into.begin(),
                                         into.begin() + static_cast<std::ptrdiff_t>(heard.bytes)),
                  *expected);
        EXPECT_EQ(heard.from.host, "127.0.0.1");
        EXPECT_EQ(heard.from.port, sender.socket.port());
    }
}

TEST(NetConnection, ADatagramLargerThanItsBufferIsTruncated) {
    net::Bound sender   = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    net::Bound receiver = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(sender.status, net::Status::Ok);
    ASSERT_EQ(receiver.status, net::Status::Ok);

    const std::vector<std::byte> large = risingPattern(32);
    ASSERT_EQ(sender.socket.send({.host = "127.0.0.1", .port = receiver.socket.port()}, large).status,
              net::Status::Ok);

    std::array<std::byte, 8> into{};
    ASSERT_EQ(receiver.socket.waitReadable(kSettle), net::Status::Ok);
    const net::Received heard = receiver.socket.receive(into);
    EXPECT_EQ(heard.status, net::Status::Truncated);
    EXPECT_EQ(heard.bytes, 8u);
    EXPECT_EQ(std::vector<std::byte>(into.begin(), into.end()),
              std::vector<std::byte>(large.begin(), large.begin() + 8));

    // The rest of that datagram is gone.
    EXPECT_EQ(receiver.socket.receive(into).status, net::Status::WouldBlock);
}

TEST(NetConnection, AQuietDatagramSocketWouldBlock) {
    net::Bound bound = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(bound.status, net::Status::Ok);

    std::array<std::byte, 8> into{};
    const net::Received      heard = bound.socket.receive(into);
    EXPECT_EQ(heard.status, net::Status::WouldBlock);
    EXPECT_EQ(heard.bytes, 0u);
}

TEST(NetConnection, AListenerOrSocketHoldingNothingAnswersClosed) {
    net::Listener listener;
    const net::Opened accepted = listener.accept();
    EXPECT_EQ(accepted.status, net::Status::Closed);
    EXPECT_EQ(accepted.stream, nullptr);
    EXPECT_EQ(listener.waitReadable(0ms), net::Status::Closed);
    EXPECT_EQ(listener.port(), 0);

    net::DatagramSocket          socket;
    const std::vector<std::byte> bytes = bytesOf({0x01});
    std::array<std::byte, 8>     into{};
    EXPECT_EQ(socket.send({.host = "127.0.0.1", .port = 9}, bytes).status, net::Status::Closed);
    EXPECT_EQ(socket.receive(into).status, net::Status::Closed);
    EXPECT_EQ(socket.waitReadable(0ms), net::Status::Closed);
}

TEST(NetConnection, AMovedListenerKeepsItsPort) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);
    const std::uint16_t port = listening.listener.port();

    net::Listener moved(std::move(listening.listener));
    EXPECT_EQ(moved.port(), port);
    EXPECT_EQ(listening.listener.port(), 0);
    EXPECT_EQ(listening.listener.accept().status, net::Status::Closed);

    const net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = port});
    ASSERT_EQ(dialed.status, net::Status::Ok);
    ASSERT_EQ(moved.waitReadable(kSettle), net::Status::Ok);
    EXPECT_EQ(moved.accept().status, net::Status::Ok);
}

TEST(NetConnection, TlsOpensAgainstAPinnedCertificateAndCarriesBytes) {
    const std::vector<std::byte> identity = readFixture("loopback.p12");
    const std::vector<std::byte> pinned   = readFixture("loopback-cert.der");
    ASSERT_FALSE(identity.empty());
    ASSERT_FALSE(pinned.empty());

    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);
    const std::uint16_t port = listening.listener.port();

    TlsServerRun run;
    std::thread  server([&] { serveTls(listening.listener, identity, /*echo=*/true, run); });

    net::Opened opened = net::open(net::Tcp{.host = "127.0.0.1", .port = port, .timeout = kSettle},
                                   net::TlsClientConfig{.hostname = "localhost", .pinned = {pinned}});
    EXPECT_EQ(opened.status, net::Status::Ok);
    std::vector<std::byte> back;
    if (opened.status == net::Status::Ok) {
        const std::vector<std::byte> sent = risingPattern(48);
        EXPECT_TRUE(sendAll(*opened.stream, sent));
        back = readExactly(*opened.stream, sent.size(), sent.size());
        EXPECT_EQ(back, sent);
    }
    opened.stream.reset();
    server.join();

    EXPECT_EQ(run.handshake, net::Status::Ok);
    EXPECT_TRUE(run.echoed);
}

TEST(NetConnection, TlsOpenRefusesACertificateItDoesNotPin) {
    const std::vector<std::byte> identity = readFixture("other.p12");
    const std::vector<std::byte> pinned   = readFixture("loopback-cert.der");
    ASSERT_FALSE(identity.empty());
    ASSERT_FALSE(pinned.empty());

    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);
    const std::uint16_t port = listening.listener.port();

    TlsServerRun run;
    std::thread  server([&] { serveTls(listening.listener, identity, /*echo=*/false, run); });

    const net::Opened opened =
        net::open(net::Tcp{.host = "127.0.0.1", .port = port, .timeout = kSettle},
                  net::TlsClientConfig{.hostname = "localhost", .pinned = {pinned}});
    server.join();

    EXPECT_EQ(opened.status, net::Status::Untrusted);
    EXPECT_EQ(opened.stream, nullptr);
}

TEST(NetConnection, TlsRunsOverTheInProcessPair) {
    const std::vector<std::byte> identity = readFixture("loopback.p12");
    const std::vector<std::byte> pinned   = readFixture("loopback-cert.der");
    ASSERT_FALSE(identity.empty());
    ASSERT_FALSE(pinned.empty());

    std::unique_ptr<net::Stream> first;
    std::unique_ptr<net::Stream> second;
    ASSERT_TRUE(inProcessPair("net connection tls", first, second));

    net::Secured client = net::tlsClient(std::move(first),
                                         net::TlsClientConfig{.hostname = "localhost", .pinned = {pinned}});
    net::Secured server = net::tlsServer(std::move(second),
                                         net::TlsServerConfig{.identity = identity, .password = kPassword});
    ASSERT_EQ(client.status, net::Status::Ok);
    ASSERT_EQ(server.status, net::Status::Ok);

    // Both ends live in this thread, so neither may wait for the other: each makes what progress it can.
    const auto deadline = std::chrono::steady_clock::now() + kLiveness;
    while (!(client.stream->settled() && server.stream->settled()) &&
           std::chrono::steady_clock::now() < deadline) {
        if (!client.stream->settled()) {
            const net::Status step = client.stream->handshake(0ms);
            ASSERT_TRUE(step == net::Status::Ok || step == net::Status::WouldBlock);
        }
        if (!server.stream->settled()) {
            const net::Status step = server.stream->handshake(0ms);
            ASSERT_TRUE(step == net::Status::Ok || step == net::Status::WouldBlock);
        }
    }
    ASSERT_TRUE(client.stream->settled());
    ASSERT_TRUE(server.stream->settled());

    const std::vector<std::byte> sent = risingPattern(48);
    ASSERT_TRUE(sendAll(*client.stream, sent));
    EXPECT_EQ(readExactly(*server.stream, sent.size(), sent.size()), sent);
}

TEST(NetConnection, OpenPresentsTheRowsHostWhenTheConfigNamesNone) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    ASSERT_EQ(listening.status, net::Status::Ok);
    const std::uint16_t port = listening.listener.port();

    // The config names no host, so the client presents the row's.
    net::Opened clientSide;
    std::thread client([&] {
        clientSide = net::open(net::Tcp{.host = "localhost", .port = port, .timeout = kSettle},
                               net::TlsClientConfig{});
    });

    // A plain listener reads what the client sends first. The name a client presents crosses in the
    // clear, before any certificate, so no TLS server is needed to see it.
    std::vector<std::byte> heard;
    if (listening.listener.waitReadable(kSettle) == net::Status::Ok) {
        net::Opened accepted = listening.listener.accept();
        if (accepted.status == net::Status::Ok) {
            std::array<std::byte, 4096> into{};
            const auto                  deadline = std::chrono::steady_clock::now() + kSettle;
            while (!contains(heard, "localhost") && std::chrono::steady_clock::now() < deadline) {
                const net::Transfer moved = accepted.stream->receive(into);
                if (moved.status == net::Status::Ok) {
                    heard.insert(heard.end(), into.begin(),
                                 into.begin() + static_cast<std::ptrdiff_t>(moved.bytes));
                    continue;
                }
                if (moved.status == net::Status::WouldBlock) {
                    (void)accepted.stream->waitReadable(100ms);
                    continue;
                }
                break;
            }
        }
        // The accepted stream is destroyed here, and the client's handshake loses its peer.
    }
    client.join();

    EXPECT_TRUE(contains(heard, "localhost"));
    // Which status a client answers when its peer leaves mid-handshake is the TLS library's.
    EXPECT_NE(clientSide.status, net::Status::Ok);
    EXPECT_EQ(clientSide.stream, nullptr);
}

TEST(NetConnection, TlsOverNoStreamAnswersClosedWithNoStream) {
    const net::Secured client = net::tlsClient(nullptr, net::TlsClientConfig{});
    ASSERT_EQ(client.status, net::Status::Closed);
    EXPECT_EQ(client.stream, nullptr);

    const net::Secured server = net::tlsServer(nullptr, net::TlsServerConfig{});
    ASSERT_EQ(server.status, net::Status::Closed);
    EXPECT_EQ(server.stream, nullptr);
}

}  // namespace
