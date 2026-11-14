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

#include "retropp/net.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace retropp::net {

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
[[nodiscard]] Status connectStream(const Endpoint& to, std::chrono::milliseconds timeout,
                                   SocketHandle& out);

// Bind a stream socket and begin accepting on it. Port 0 asks the OS for an unused port; localPort
// reads back the one it chose.
[[nodiscard]] Status listenStream(const Endpoint& at, int backlog, SocketHandle& out);

// Take the next connection off a listening socket. WouldBlock means none is waiting yet.
[[nodiscard]] Status acceptStream(SocketHandle listener, SocketHandle& out);

// Bind a datagram socket. Port 0 asks the OS for an unused port.
[[nodiscard]] Status bindDatagram(const Endpoint& at, SocketHandle& out);

// The port the OS bound, which is what a caller that asked for 0 needs in order to be dialed.
[[nodiscard]] Status localPort(SocketHandle socket, std::uint16_t& out);

// What the seam set on a socket, read back from the OS: whether it sends each write as it is made, and
// whether it may send to a broadcast address. An option the OS does not carry on that kind of socket
// reads false.
struct SocketOptions {
    bool sendsAtOnce  = false;
    bool mayBroadcast = false;
};
[[nodiscard]] SocketOptions socketOptions(SocketHandle socket);

[[nodiscard]] Transfer sendStream(SocketHandle socket, std::span<const std::byte> bytes);
[[nodiscard]] Transfer receiveStream(SocketHandle socket, std::span<std::byte> into);

// One datagram per call in both directions: a send is one datagram, and a receive fills from exactly
// one and reports who sent it. Boundaries are preserved rather than coalesced, which is the difference
// that makes a datagram its own shape here instead of a setting on a stream.
[[nodiscard]] Transfer sendDatagram(SocketHandle socket, const Endpoint& to,
                                    std::span<const std::byte> bytes);
[[nodiscard]] Transfer receiveDatagram(SocketHandle socket, std::span<std::byte> into,
                                       Endpoint& from);

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
