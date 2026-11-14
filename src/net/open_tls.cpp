// Dialing a stream and bringing TLS up over it in one call.

#include "retropp/net.h"

#include <utility>

namespace retropp::net {

Opened open(const Tcp& to, const TlsClientConfig& accepting) {
    Opened plain = open(to);
    if (plain.status != Status::Ok) return plain;

    TlsClientConfig trust = accepting;
    if (trust.hostname.empty()) trust.hostname = to.host;

    Secured secured = tlsClient(std::move(plain.stream), trust);
    if (secured.status != Status::Ok) return Opened{.status = secured.status, .stream = nullptr};

    // The handshake gets the row's timeout to settle in.
    const auto deadline = std::chrono::steady_clock::now() + to.timeout;
    for (;;) {
        const auto now  = std::chrono::steady_clock::now();
        const auto left = now < deadline
                              ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                              : std::chrono::milliseconds{0};

        const Status step = secured.stream->handshake(left);
        if (step == Status::Ok) return Opened{.status = Status::Ok, .stream = std::move(secured.stream)};
        if (step != Status::WouldBlock) return Opened{.status = step, .stream = nullptr};
        if (left == std::chrono::milliseconds{0}) {
            return Opened{.status = Status::TimedOut, .stream = nullptr};
        }
    }
}

}  // namespace retropp::net
