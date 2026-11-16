// A program that dials a plain stream and binds a datagram socket, and names no TLS.
//
// net_linkage.cmake reads this binary twice: against a program that names no transport, where the socket
// seam's symbols belong here and stay out of that one, and against the test binary, where the OS's TLS
// library is recorded there and stays out of this one.

#include <retropp/net.h>

#include <cstdio>

int main() {
    using namespace retropp::net;

    // Nothing listens on port 1 of the loopback, so the dial is answered at once.
    const Opened dialed = open(Tcp{.host = "127.0.0.1", .port = 1, .timeout = std::chrono::milliseconds{50}});
    const Bound  bound  = bindDatagram(Endpoint{.host = "127.0.0.1", .port = 0});

    std::printf("%d %d\n", static_cast<int>(dialed.status), static_cast<int>(bound.status));
    return 0;
}
