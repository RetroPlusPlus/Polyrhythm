// Streams, listeners and datagram sockets as a game holds them: each owns what the OS handed out and
// answers with a value.

#include "retropp/net.h"

#include "src/net/in_process.h"
#include "src/net/socket.h"

#include <utility>

namespace retropp::net {
namespace {

SocketHandle handleOf(std::uint64_t value) noexcept { return SocketHandle{.value = value}; }

void release(std::uint64_t& value) noexcept {
    SocketHandle socket = handleOf(value);
    closeSocket(socket);
    value = kNoSocket;
}

// An unnamed host is every interface's IPv4 address, so one call binds the same family on every OS.
Endpoint named(const Endpoint& at) {
    return at.host.empty() ? Endpoint{.host = "0.0.0.0", .port = at.port} : at;
}

}  // namespace

Opened open(const Tcp& to) {
    SocketHandle socket;
    const Status connected =
        connectStream(Endpoint{.host = to.host, .port = to.port}, to.timeout, socket);
    if (connected != Status::Ok) return Opened{.status = connected, .stream = nullptr};
    return Opened{.status = Status::Ok, .stream = std::make_unique<SocketStream>(socket)};
}

Opened open(const InProcess& at) {
    auto end = std::make_unique<InProcessStream>();
    if (const Status joined = openInProcessStream(at.pair, *end); joined != Status::Ok) {
        return Opened{.status = joined, .stream = nullptr};
    }
    return Opened{.status = Status::Ok, .stream = std::move(end)};
}

// ── Listener ────────────────────────────────────────────────────────────────────

Listener::~Listener() { release(handle_); }

Listener::Listener(Listener&& other) noexcept
    : handle_(std::exchange(other.handle_, kNoSocket)),
      port_(std::exchange(other.port_, std::uint16_t{0})) {}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        release(handle_);
        handle_ = std::exchange(other.handle_, kNoSocket);
        port_   = std::exchange(other.port_, std::uint16_t{0});
    }
    return *this;
}

Opened Listener::accept() {
    if (handle_ == kNoSocket) return Opened{.status = Status::Closed, .stream = nullptr};
    SocketHandle accepted;
    const Status taken = acceptStream(handleOf(handle_), accepted);
    if (taken != Status::Ok) return Opened{.status = taken, .stream = nullptr};
    return Opened{.status = Status::Ok, .stream = std::make_unique<SocketStream>(accepted)};
}

Status Listener::waitReadable(std::chrono::milliseconds timeout) {
    if (handle_ == kNoSocket) return Status::Closed;
    return net::waitReadable(handleOf(handle_), timeout);
}

Listening listen(const Endpoint& at, int backlog) {
    SocketHandle socket;
    if (const Status bound = listenStream(named(at), backlog, socket); bound != Status::Ok) {
        return Listening{.status = bound, .listener = {}};
    }

    Listening result{.status = Status::Ok, .listener = {}};
    result.listener.handle_ = socket.value;
    if (const Status read = localPort(socket, result.listener.port_); read != Status::Ok) {
        // result's listener owns the socket and closes it on the way out.
        return Listening{.status = read, .listener = {}};
    }
    return result;
}

// ── DatagramSocket ──────────────────────────────────────────────────────────────

DatagramSocket::~DatagramSocket() { release(handle_); }

DatagramSocket::DatagramSocket(DatagramSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kNoSocket)),
      port_(std::exchange(other.port_, std::uint16_t{0})) {}

DatagramSocket& DatagramSocket::operator=(DatagramSocket&& other) noexcept {
    if (this != &other) {
        release(handle_);
        handle_ = std::exchange(other.handle_, kNoSocket);
        port_   = std::exchange(other.port_, std::uint16_t{0});
    }
    return *this;
}

Transfer DatagramSocket::send(const Endpoint& to, std::span<const std::byte> bytes) {
    if (handle_ == kNoSocket) return Transfer{.status = Status::Closed, .bytes = 0};
    return sendDatagram(handleOf(handle_), to, bytes);
}

Received DatagramSocket::receive(std::span<std::byte> into) {
    if (handle_ == kNoSocket) return Received{.status = Status::Closed, .bytes = 0, .from = {}};
    Received       result{};
    const Transfer moved = receiveDatagram(handleOf(handle_), into, result.from);
    result.status        = moved.status;
    result.bytes         = moved.bytes;
    return result;
}

Status DatagramSocket::waitReadable(std::chrono::milliseconds timeout) {
    if (handle_ == kNoSocket) return Status::Closed;
    return net::waitReadable(handleOf(handle_), timeout);
}

Bound bindDatagram(const Endpoint& at) {
    SocketHandle socket;
    if (const Status bound = bindDatagram(named(at), socket); bound != Status::Ok) {
        return Bound{.status = bound, .socket = {}};
    }

    Bound result{.status = Status::Ok, .socket = {}};
    result.socket.handle_ = socket.value;
    if (const Status read = localPort(socket, result.socket.port_); read != Status::Ok) {
        // result's socket owns the handle and closes it on the way out.
        return Bound{.status = read, .socket = {}};
    }
    return result;
}

}  // namespace retropp::net
