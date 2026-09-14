// The transport seam on Windows: Winsock.
//
// The OS headers stop here, so <winsock2.h> — and the macro weather behind it — never reaches a
// translation unit that merely touches the seam.
//
// Winsock's lifetime is owned here and nowhere else: the count below rises with the first socket this
// seam opens and falls with the last one closed, so a game that opens none runs no WSAStartup. A global
// constructor or an init call a consumer had to remember would both cost that property.
//
// Readiness is measured with select rather than WSAPoll: WSAPoll does not report a failed connection
// attempt, which is exactly the answer a connect to a closed port owes its caller.

#include "src/net/socket.h"

#if defined(_WIN32)

// Ask <windows.h> for the modern socket header only, and leave the min/max macros out of the way.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
// ws2tcpip.h must follow winsock2.h.
#include <ws2tcpip.h>

#include <cstdlib>
#include <mutex>
#include <string>

namespace retropp::net {
namespace {

// Function-local storage, so there is no static initialization order to reason about.
std::mutex& lifetimeMutex() {
    static std::mutex mutex;
    return mutex;
}

std::size_t& liveSockets() {
    static std::size_t count = 0;
    return count;
}

[[nodiscard]] bool acquireWinsock() {
    const std::lock_guard<std::mutex> guard(lifetimeMutex());
    if (liveSockets() == 0) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    }
    ++liveSockets();
    return true;
}

void releaseWinsock() {
    const std::lock_guard<std::mutex> guard(lifetimeMutex());
    if (liveSockets() == 0) return;
    if (--liveSockets() == 0) ::WSACleanup();
}

// Holds the library open for the duration of one call. A socket that outlives the call takes a lease of
// its own before returning, so the count never reaches zero while a socket is open.
class WinsockScope {
public:
    WinsockScope() : held_(acquireWinsock()) {}
    ~WinsockScope() {
        if (held_) releaseWinsock();
    }

    WinsockScope(const WinsockScope&)            = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;

    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    bool held_ = false;
};

Status statusFromWsa(int code) {
    switch (code) {
        case 0:                  return Status::Ok;
        case WSAEWOULDBLOCK:
        case WSAEINPROGRESS:
        case WSAEALREADY:        return Status::WouldBlock;
        case WSAECONNREFUSED:    return Status::Refused;
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case WSAENETDOWN:
        case WSAEHOSTDOWN:       return Status::Unreachable;
        case WSAETIMEDOUT:       return Status::TimedOut;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENOTCONN:
        case WSAESHUTDOWN:       return Status::Closed;
        default:                 return Status::Other;
    }
}

Status lastStatus() { return statusFromWsa(::WSAGetLastError()); }

class Resolution {
public:
    ~Resolution() {
        if (list_ != nullptr) ::freeaddrinfo(list_);
    }

    Resolution()                             = default;
    Resolution(const Resolution&)            = delete;
    Resolution& operator=(const Resolution&) = delete;

    [[nodiscard]] bool resolve(const SocketAddress& address, bool passive, int socktype) {
        ::addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;  // v4 and v6 both; the OS orders the candidates
        hints.ai_socktype = socktype;
        hints.ai_flags    = passive ? AI_PASSIVE : 0;

        const std::string port = std::to_string(address.port);
        const char* host       = address.host.empty() ? nullptr : address.host.c_str();
        return ::getaddrinfo(host, port.c_str(), &hints, &list_) == 0 && list_ != nullptr;
    }

    [[nodiscard]] const ::addrinfo* candidates() const noexcept { return list_; }

private:
    ::addrinfo* list_ = nullptr;
};

SOCKET socketOf(SocketHandle socket) noexcept { return static_cast<SOCKET>(socket.value); }

SocketHandle handleOf(SOCKET socket) noexcept {
    return SocketHandle{.value = static_cast<std::uint64_t>(socket)};
}

bool makeNonBlocking(SOCKET socket) {
    u_long nonBlocking = 1;
    return ::ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
}

// Closes a socket that has not taken a lease of its own yet.
void discard(SOCKET socket) noexcept { ::closesocket(socket); }

::timeval asTimeval(std::chrono::milliseconds timeout) {
    ::timeval span{};
    span.tv_sec  = static_cast<long>(timeout.count() / 1000);
    span.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return span;
}

// A connect in flight lands in the writable set on success and in the exceptional set on failure; the
// pending error tells which, and a refused port is the answer that matters most here.
Status waitConnected(SOCKET socket, std::chrono::milliseconds timeout) {
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);

    ::timeval span  = asTimeval(timeout);
    const int ready = ::select(0, nullptr, &writable, &failed, &span);
    if (ready == SOCKET_ERROR) return lastStatus();
    if (ready == 0) return Status::TimedOut;

    int pending = 0;
    int size    = static_cast<int>(sizeof(pending));
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &size) ==
        SOCKET_ERROR) {
        return lastStatus();
    }
    return statusFromWsa(pending);
}

int transferLength(std::size_t size) noexcept {
    // Winsock counts bytes in an int; a larger span moves in more than one call, which a stream caller
    // already handles through the returned count.
    constexpr std::size_t kCeiling = 1 << 20;
    return static_cast<int>(size < kCeiling ? size : kCeiling);
}

void addressFrom(const ::sockaddr* storage, int length, SocketAddress& out) {
    char host[NI_MAXHOST] = {};
    char port[NI_MAXSERV] = {};
    if (::getnameinfo(storage, length, host, sizeof(host), port, sizeof(port),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        out.host = host;
        out.port = static_cast<std::uint16_t>(std::strtoul(port, nullptr, 10));
    }
}

}  // namespace

Status connectStream(const SocketAddress& to, std::chrono::milliseconds timeout, SocketHandle& out) {
    const WinsockScope scope;
    if (!scope.held()) return Status::Other;

    Resolution resolution;
    if (!resolution.resolve(to, /*passive=*/false, SOCK_STREAM)) return Status::Unreachable;

    Status last = Status::Unreachable;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const SOCKET socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                       candidate->ai_protocol);
        if (socket == INVALID_SOCKET) {
            last = lastStatus();
            continue;
        }
        if (!makeNonBlocking(socket)) {
            last = lastStatus();
            discard(socket);
            continue;
        }

        Status settled = Status::Ok;
        if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) ==
            SOCKET_ERROR) {
            const Status immediate = lastStatus();
            settled = (immediate == Status::WouldBlock) ? waitConnected(socket, timeout) : immediate;
        }

        if (settled == Status::Ok) {
            if (!acquireWinsock()) {
                discard(socket);
                return Status::Other;
            }
            out = handleOf(socket);
            return Status::Ok;
        }
        last = settled;
        discard(socket);
    }
    return last;
}

Status listenStream(const SocketAddress& at, int backlog, SocketHandle& out) {
    const WinsockScope scope;
    if (!scope.held()) return Status::Other;

    Resolution resolution;
    if (!resolution.resolve(at, /*passive=*/true, SOCK_STREAM)) return Status::Other;

    Status last = Status::Other;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const SOCKET socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                       candidate->ai_protocol);
        if (socket == INVALID_SOCKET) {
            last = lastStatus();
            continue;
        }

        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            ::listen(socket, backlog) == 0 && makeNonBlocking(socket)) {
            if (!acquireWinsock()) {
                discard(socket);
                return Status::Other;
            }
            out = handleOf(socket);
            return Status::Ok;
        }
        last = lastStatus();
        discard(socket);
    }
    return last;
}

Status acceptStream(SocketHandle listener, SocketHandle& out) {
    const SOCKET accepted = ::accept(socketOf(listener), nullptr, nullptr);
    if (accepted == INVALID_SOCKET) return lastStatus();
    if (!makeNonBlocking(accepted)) {
        const Status why = lastStatus();
        discard(accepted);
        return why;
    }
    if (!acquireWinsock()) {
        discard(accepted);
        return Status::Other;
    }
    out = handleOf(accepted);
    return Status::Ok;
}

Status bindDatagram(const SocketAddress& at, SocketHandle& out) {
    const WinsockScope scope;
    if (!scope.held()) return Status::Other;

    Resolution resolution;
    if (!resolution.resolve(at, /*passive=*/true, SOCK_DGRAM)) return Status::Other;

    Status last = Status::Other;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const SOCKET socket = ::socket(candidate->ai_family, candidate->ai_socktype,
                                       candidate->ai_protocol);
        if (socket == INVALID_SOCKET) {
            last = lastStatus();
            continue;
        }

        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            makeNonBlocking(socket)) {
            if (!acquireWinsock()) {
                discard(socket);
                return Status::Other;
            }
            out = handleOf(socket);
            return Status::Ok;
        }
        last = lastStatus();
        discard(socket);
    }
    return last;
}

Status localPort(SocketHandle socket, std::uint16_t& out) {
    ::sockaddr_storage bound{};
    int                size = static_cast<int>(sizeof(bound));
    if (::getsockname(socketOf(socket), reinterpret_cast<::sockaddr*>(&bound), &size) ==
        SOCKET_ERROR) {
        return lastStatus();
    }

    if (bound.ss_family == AF_INET) {
        out = ::ntohs(reinterpret_cast<const ::sockaddr_in*>(&bound)->sin_port);
        return Status::Ok;
    }
    if (bound.ss_family == AF_INET6) {
        out = ::ntohs(reinterpret_cast<const ::sockaddr_in6*>(&bound)->sin6_port);
        return Status::Ok;
    }
    return Status::Other;
}

Transfer sendStream(SocketHandle socket, std::span<const std::byte> bytes) {
    const int moved = ::send(socketOf(socket), reinterpret_cast<const char*>(bytes.data()),
                             transferLength(bytes.size()), 0);
    if (moved == SOCKET_ERROR) return Transfer{.status = lastStatus(), .bytes = 0};
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Transfer receiveStream(SocketHandle socket, std::span<std::byte> into) {
    const int moved = ::recv(socketOf(socket), reinterpret_cast<char*>(into.data()),
                             transferLength(into.size()), 0);
    if (moved == SOCKET_ERROR) return Transfer{.status = lastStatus(), .bytes = 0};
    // Zero bytes from a stream is the peer's orderly close, not an empty read.
    if (moved == 0) return Transfer{.status = Status::Closed, .bytes = 0};
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Transfer sendDatagram(SocketHandle socket, const SocketAddress& to,
                      std::span<const std::byte> bytes) {
    Resolution resolution;
    if (!resolution.resolve(to, /*passive=*/false, SOCK_DGRAM)) {
        return Transfer{.status = Status::Unreachable, .bytes = 0};
    }

    const ::addrinfo* target = resolution.candidates();
    const int         moved  = ::sendto(socketOf(socket), reinterpret_cast<const char*>(bytes.data()),
                                        transferLength(bytes.size()), 0, target->ai_addr,
                                        static_cast<int>(target->ai_addrlen));
    if (moved == SOCKET_ERROR) return Transfer{.status = lastStatus(), .bytes = 0};
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Transfer receiveDatagram(SocketHandle socket, std::span<std::byte> into, SocketAddress& from) {
    ::sockaddr_storage sender{};
    int                size  = static_cast<int>(sizeof(sender));
    const int          moved = ::recvfrom(socketOf(socket), reinterpret_cast<char*>(into.data()),
                                          transferLength(into.size()), 0,
                                          reinterpret_cast<::sockaddr*>(&sender), &size);
    if (moved == SOCKET_ERROR) return Transfer{.status = lastStatus(), .bytes = 0};

    addressFrom(reinterpret_cast<const ::sockaddr*>(&sender), size, from);
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Status waitReadable(SocketHandle socket, std::chrono::milliseconds timeout) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socketOf(socket), &readable);

    ::timeval span  = asTimeval(timeout);
    const int ready = ::select(0, &readable, nullptr, nullptr, &span);
    if (ready == SOCKET_ERROR) return lastStatus();
    if (ready == 0) return Status::TimedOut;
    return Status::Ok;
}

void closeSocket(SocketHandle& socket) noexcept {
    if (socket.valid()) {
        ::closesocket(socketOf(socket));
        socket = SocketHandle{};
        releaseWinsock();
    }
}

}  // namespace retropp::net

#endif  // _WIN32
