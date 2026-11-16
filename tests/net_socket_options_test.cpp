// What the socket seam sets on every socket it hands out, read back from the OS: a stream sends each
// write as it is made, on the end that dialed and on the end that was accepted, and a datagram socket
// may send to a broadcast address.

#include "src/net/socket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace {

using namespace std::chrono_literals;

using retropp::net::acceptStream;
using retropp::net::bindDatagram;
using retropp::net::closeSocket;
using retropp::net::connectStream;
using retropp::net::listenStream;
using retropp::net::localPort;
using retropp::net::SocketHandle;
using retropp::net::socketOptions;
using retropp::net::Status;

constexpr auto kSettle = 5000ms;

TEST(NetSocketOptions, BothEndsOfAStreamSendAtOnce) {
    SocketHandle listener;
    ASSERT_EQ(listenStream({.host = "127.0.0.1", .port = 0}, 4, listener), Status::Ok);
    std::uint16_t port = 0;
    ASSERT_EQ(localPort(listener, port), Status::Ok);

    SocketHandle dialed;
    ASSERT_EQ(connectStream({.host = "127.0.0.1", .port = port}, kSettle, dialed), Status::Ok);
    ASSERT_EQ(retropp::net::waitReadable(listener, kSettle), Status::Ok);
    SocketHandle accepted;
    ASSERT_EQ(acceptStream(listener, accepted), Status::Ok);

    EXPECT_TRUE(socketOptions(dialed).sendsAtOnce);
    EXPECT_TRUE(socketOptions(accepted).sendsAtOnce);

    closeSocket(accepted);
    closeSocket(dialed);
    closeSocket(listener);
}

TEST(NetSocketOptions, ADatagramSocketMayBroadcast) {
    SocketHandle socket;
    ASSERT_EQ(bindDatagram({.host = "127.0.0.1", .port = 0}, socket), Status::Ok);

    EXPECT_TRUE(socketOptions(socket).mayBroadcast);

    closeSocket(socket);
}

}  // namespace
