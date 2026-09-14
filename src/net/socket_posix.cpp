// The transport seam on macOS and Linux: BSD sockets.
//
// The OS headers stop here. Every errno the platform can tell apart is mapped to a Status before it
// returns, so the code above this file reads no errno and includes no <sys/socket.h>.

#include "src/net/socket.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace retropp::net {
namespace {

// A quiet socket, a full send window and a connect still in flight all mean "come back later", and the
// caller distinguishes them by which call it made.
Status statusFromErrno(int code) {
    switch (code) {
        case 0:              return Status::Ok;
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS:
        case EALREADY:       return Status::WouldBlock;
        case ECONNREFUSED:   return Status::Refused;
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ENETDOWN:
        case EHOSTDOWN:      return Status::Unreachable;
        case ETIMEDOUT:      return Status::TimedOut;
        case EPIPE:
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
        case ESHUTDOWN:      return Status::Closed;
        default:             return Status::Other;
    }
}

// Closes the addrinfo list however the function leaves.
class Resolution {
public:
    ~Resolution() {
        if (list_ != nullptr) ::freeaddrinfo(list_);
    }

    Resolution()                             = default;
    Resolution(const Resolution&)            = delete;
    Resolution& operator=(const Resolution&) = delete;

    // Resolves a host and port for either family. A host left empty means "every interface" when
    // passive and the loopback when not, which is what the two callers each want by default.
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

int descriptorOf(SocketHandle socket) noexcept { return static_cast<int>(socket.value); }

SocketHandle handleOf(int descriptor) noexcept {
    return SocketHandle{.value = static_cast<std::uint64_t>(descriptor)};
}

// Every socket the seam hands out is non-blocking, and on Apple it also refuses to raise SIGPIPE —
// a write to a closed peer is an answer this seam returns, never a signal that ends the process.
bool makeNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return false;
    if (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) return false;
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return true;
}

// Linux carries the same guarantee as a per-call flag instead of a socket option.
constexpr int kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif

// Reads a bound or peer sockaddr back into the presentation form the seam speaks.
void addressFrom(const ::sockaddr* storage, ::socklen_t length, SocketAddress& out) {
    char host[NI_MAXHOST] = {};
    char port[NI_MAXSERV] = {};
    if (::getnameinfo(storage, length, host, sizeof(host), port, sizeof(port),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        out.host = host;
        out.port = static_cast<std::uint16_t>(std::strtoul(port, nullptr, 10));
    }
}

Status waitWritable(int descriptor, std::chrono::milliseconds timeout) {
    ::pollfd waiting{};
    waiting.fd     = descriptor;
    waiting.events = POLLOUT;

    const int ready = ::poll(&waiting, 1, static_cast<int>(timeout.count()));
    if (ready < 0) return statusFromErrno(errno);
    if (ready == 0) return Status::TimedOut;

    // A refused or unreachable connect reports POLLOUT too; the pending error is the real answer.
    int       pending = 0;
    socklen_t size    = sizeof(pending);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &size) < 0) {
        return statusFromErrno(errno);
    }
    return statusFromErrno(pending);
}

}  // namespace

Status connectStream(const SocketAddress& to, std::chrono::milliseconds timeout, SocketHandle& out) {
    Resolution resolution;
    if (!resolution.resolve(to, /*passive=*/false, SOCK_STREAM)) return Status::Unreachable;

    Status last = Status::Unreachable;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const int descriptor = ::socket(candidate->ai_family, candidate->ai_socktype,
                                        candidate->ai_protocol);
        if (descriptor < 0) {
            last = statusFromErrno(errno);
            continue;
        }
        if (!makeNonBlocking(descriptor)) {
            last = statusFromErrno(errno);
            ::close(descriptor);
            continue;
        }

        Status settled = Status::Ok;
        if (::connect(descriptor, candidate->ai_addr, candidate->ai_addrlen) < 0) {
            const Status immediate = statusFromErrno(errno);
            settled = (immediate == Status::WouldBlock) ? waitWritable(descriptor, timeout) : immediate;
        }

        if (settled == Status::Ok) {
            out = handleOf(descriptor);
            return Status::Ok;
        }
        last = settled;
        ::close(descriptor);
    }
    return last;
}

Status listenStream(const SocketAddress& at, int backlog, SocketHandle& out) {
    Resolution resolution;
    if (!resolution.resolve(at, /*passive=*/true, SOCK_STREAM)) return Status::Other;

    Status last = Status::Other;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const int descriptor = ::socket(candidate->ai_family, candidate->ai_socktype,
                                        candidate->ai_protocol);
        if (descriptor < 0) {
            last = statusFromErrno(errno);
            continue;
        }

        int on = 1;
        ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        if (::bind(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
            ::listen(descriptor, backlog) == 0 && makeNonBlocking(descriptor)) {
            out = handleOf(descriptor);
            return Status::Ok;
        }
        last = statusFromErrno(errno);
        ::close(descriptor);
    }
    return last;
}

Status acceptStream(SocketHandle listener, SocketHandle& out) {
    const int accepted = ::accept(descriptorOf(listener), nullptr, nullptr);
    if (accepted < 0) return statusFromErrno(errno);
    if (!makeNonBlocking(accepted)) {
        const Status why = statusFromErrno(errno);
        ::close(accepted);
        return why;
    }
    out = handleOf(accepted);
    return Status::Ok;
}

Status bindDatagram(const SocketAddress& at, SocketHandle& out) {
    Resolution resolution;
    if (!resolution.resolve(at, /*passive=*/true, SOCK_DGRAM)) return Status::Other;

    Status last = Status::Other;
    for (const ::addrinfo* candidate = resolution.candidates(); candidate != nullptr;
         candidate                   = candidate->ai_next) {
        const int descriptor = ::socket(candidate->ai_family, candidate->ai_socktype,
                                        candidate->ai_protocol);
        if (descriptor < 0) {
            last = statusFromErrno(errno);
            continue;
        }

        int on = 1;
        ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        if (::bind(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
            makeNonBlocking(descriptor)) {
            out = handleOf(descriptor);
            return Status::Ok;
        }
        last = statusFromErrno(errno);
        ::close(descriptor);
    }
    return last;
}

Status localPort(SocketHandle socket, std::uint16_t& out) {
    ::sockaddr_storage bound{};
    ::socklen_t        size = sizeof(bound);
    if (::getsockname(descriptorOf(socket), reinterpret_cast<::sockaddr*>(&bound), &size) < 0) {
        return statusFromErrno(errno);
    }

    if (bound.ss_family == AF_INET) {
        out = ntohs(reinterpret_cast<const ::sockaddr_in*>(&bound)->sin_port);
        return Status::Ok;
    }
    if (bound.ss_family == AF_INET6) {
        out = ntohs(reinterpret_cast<const ::sockaddr_in6*>(&bound)->sin6_port);
        return Status::Ok;
    }
    return Status::Other;
}

Transfer sendStream(SocketHandle socket, std::span<const std::byte> bytes) {
    const ::ssize_t moved = ::send(descriptorOf(socket), bytes.data(), bytes.size(), kSendFlags);
    if (moved < 0) return Transfer{.status = statusFromErrno(errno), .bytes = 0};
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Transfer receiveStream(SocketHandle socket, std::span<std::byte> into) {
    const ::ssize_t moved = ::recv(descriptorOf(socket), into.data(), into.size(), 0);
    if (moved < 0) return Transfer{.status = statusFromErrno(errno), .bytes = 0};
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
    const ::ssize_t   moved  = ::sendto(descriptorOf(socket), bytes.data(), bytes.size(), kSendFlags,
                                        target->ai_addr, target->ai_addrlen);
    if (moved < 0) return Transfer{.status = statusFromErrno(errno), .bytes = 0};
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Transfer receiveDatagram(SocketHandle socket, std::span<std::byte> into, SocketAddress& from) {
    ::sockaddr_storage sender{};
    ::socklen_t        size  = sizeof(sender);
    const ::ssize_t    moved = ::recvfrom(descriptorOf(socket), into.data(), into.size(), 0,
                                          reinterpret_cast<::sockaddr*>(&sender), &size);
    if (moved < 0) return Transfer{.status = statusFromErrno(errno), .bytes = 0};

    addressFrom(reinterpret_cast<const ::sockaddr*>(&sender), size, from);
    return Transfer{.status = Status::Ok, .bytes = static_cast<std::size_t>(moved)};
}

Status waitReadable(SocketHandle socket, std::chrono::milliseconds timeout) {
    ::pollfd waiting{};
    waiting.fd     = descriptorOf(socket);
    waiting.events = POLLIN;

    const int ready = ::poll(&waiting, 1, static_cast<int>(timeout.count()));
    if (ready < 0) return statusFromErrno(errno);
    if (ready == 0) return Status::TimedOut;
    return Status::Ok;
}

void closeSocket(SocketHandle& socket) noexcept {
    if (socket.valid()) {
        ::close(descriptorOf(socket));
        socket = SocketHandle{};
    }
}

}  // namespace retropp::net

#endif  // !_WIN32
