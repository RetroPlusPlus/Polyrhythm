// The transport seam on macOS and Linux: BSD sockets.
//
// The OS headers stop here. Every errno the platform can tell apart is mapped to a Status before it
// returns, so the code above this file reads no errno and includes no <sys/socket.h>.

#include "src/net/socket.h"

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

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
    [[nodiscard]] bool resolve(const Endpoint& address, bool passive, int socktype) {
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

// A stream sends each write as it is made. Left to itself the OS may hold a small write back to join it
// to the next one, which a byte exchange between two games feels as a stall.
void sendAtOnce(int descriptor) {
    int on = 1;
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

// How many leading bits of a netmask are set.
int prefixOf(const unsigned char* mask, std::size_t length) {
    int bits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        for (unsigned char byte = mask[i]; byte != 0; byte = static_cast<unsigned char>(byte << 1)) ++bits;
    }
    return bits;
}

// Linux carries the same guarantee as a per-call flag instead of a socket option.
constexpr int kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif

// Reads a bound or peer sockaddr back into the presentation form the seam speaks.
void addressFrom(const ::sockaddr* storage, ::socklen_t length, Endpoint& out) {
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

Status connectStream(const Endpoint& to, std::chrono::milliseconds timeout, SocketHandle& out) {
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
            sendAtOnce(descriptor);
            out = handleOf(descriptor);
            return Status::Ok;
        }
        last = settled;
        ::close(descriptor);
    }
    return last;
}

Status listenStream(const Endpoint& at, int backlog, SocketHandle& out) {
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
    sendAtOnce(accepted);
    out = handleOf(accepted);
    return Status::Ok;
}

Status bindDatagram(const Endpoint& at, SocketHandle& out) {
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
        // A datagram socket may send to a broadcast address, which is how one machine reaches every other
        // on its network.
        ::setsockopt(descriptor, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

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

Transfer sendDatagram(SocketHandle socket, const Endpoint& to,
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

Transfer receiveDatagram(SocketHandle socket, std::span<std::byte> into, Endpoint& from) {
    // recvmsg rather than recvfrom: it reports whether the datagram fit. recvfrom copies what it can and
    // says nothing about the rest, which would hand a caller a short count indistinguishable from a
    // short datagram.
    ::sockaddr_storage sender{};
    ::iovec            buffer{};
    buffer.iov_base = into.data();
    buffer.iov_len  = into.size();

    ::msghdr message{};
    message.msg_name    = &sender;
    message.msg_namelen = sizeof(sender);
    message.msg_iov     = &buffer;
    message.msg_iovlen  = 1;

    const ::ssize_t moved = ::recvmsg(descriptorOf(socket), &message, 0);
    if (moved < 0) return Transfer{.status = statusFromErrno(errno), .bytes = 0};

    addressFrom(reinterpret_cast<const ::sockaddr*>(&sender), message.msg_namelen, from);

    const bool fitted = (message.msg_flags & MSG_TRUNC) == 0;
    return Transfer{.status = fitted ? Status::Ok : Status::Truncated,
                    .bytes  = static_cast<std::size_t>(moved)};
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

SocketOptions socketOptions(SocketHandle socket) {
    SocketOptions read{};
    int           value = 0;
    socklen_t     size  = sizeof(value);
    read.mayBroadcast = ::getsockopt(descriptorOf(socket), SOL_SOCKET, SO_BROADCAST, &value, &size) == 0 &&
                        value != 0;
    value = 0;
    size  = sizeof(value);
    read.sendsAtOnce = ::getsockopt(descriptorOf(socket), IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 &&
                       value != 0;
    return read;
}

std::vector<Interface> localAddresses() {
    std::vector<Interface> found;
    ::ifaddrs*             list = nullptr;
    if (::getifaddrs(&list) != 0 || list == nullptr) return found;

    for (const ::ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || (entry->ifa_flags & IFF_UP) == 0) continue;
        const int family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;

        const ::socklen_t length = family == AF_INET ? sizeof(::sockaddr_in) : sizeof(::sockaddr_in6);
        char              host[NI_MAXHOST] = {};
        if (::getnameinfo(entry->ifa_addr, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) {
            continue;
        }

        int prefix = 0;
        if (entry->ifa_netmask != nullptr) {
            if (family == AF_INET) {
                const auto* mask = reinterpret_cast<const ::sockaddr_in*>(entry->ifa_netmask);
                prefix = prefixOf(reinterpret_cast<const unsigned char*>(&mask->sin_addr), 4);
            } else {
                const auto* mask = reinterpret_cast<const ::sockaddr_in6*>(entry->ifa_netmask);
                prefix = prefixOf(reinterpret_cast<const unsigned char*>(&mask->sin6_addr), 16);
            }
        }

        found.push_back(Interface{.name         = entry->ifa_name,
                                  .address      = host,
                                  .prefixLength = prefix,
                                  .loopback     = (entry->ifa_flags & IFF_LOOPBACK) != 0});
    }
    ::freeifaddrs(list);
    return found;
}

void closeSocket(SocketHandle& socket) noexcept {
    if (socket.valid()) {
        ::close(descriptorOf(socket));
        socket = SocketHandle{};
    }
}

}  // namespace retropp::net

#endif  // !_WIN32
