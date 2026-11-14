# Networking — streams, datagrams and TLS

`net.h`

`<retropp/net.h>` is the platform's networking surface, in namespace `retropp::net`: a byte stream
between two parties, over TCP or over a pair inside one process; a listener that accepts streams; a
datagram socket; TLS over any stream; and the helpers a game composes them with — an address read from
text, the addresses this machine holds, an interface's broadcast address, and the wait between attempts.
A game links it with the platform and configures nothing. A
binary carries the transport when its code calls it, and the operating system's TLS library when its code
names TLS.

```cpp
#include "retropp/net.h"

namespace net = retropp::net;

net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = listening.listener.port()});
```

## Contents

- [Every call answers at once](#every-call-answers-at-once)
- [Opening a stream](#opening-a-stream)
- [Listening](#listening)
- [The in-process pair](#the-in-process-pair)
- [What a call answers](#what-a-call-answers)
- [Datagrams](#datagrams)
- [TLS](#tls)
- [Addresses](#addresses)
- [Waiting between attempts](#waiting-between-attempts)
- [Related pages & where to change things](#related-pages--where-to-change-things)

## Every call answers at once

Every call returns without waiting for the network. A transfer that cannot proceed — nothing has arrived,
or the send window is full — answers `WouldBlock` and moves nothing, and the caller comes back to it: on
its next tick, or after `waitReadable(timeout)`, which waits up to the time it is given for something to
read. Two calls have a round trip inside them, opening a TCP stream and a TLS handshake, and each takes
the longest it may wait as a value: `Tcp::timeout` and `TlsStream::handshake(timeout)`.

A read that wants a fixed number of bytes loops on that answer:

```cpp
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
```

A short count is ordinary on a stream. `send` and `receive` answer a `Transfer` — `status`, and `bytes`,
how many moved — and the rest goes in a later call:

```cpp
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
```

One thread uses a stream, a listener or a datagram socket at a time. Each may be handed to another thread
whole: a stream through the `std::unique_ptr` that holds it, a listener or a datagram socket by moving it.

## Opening a stream

A row names the transport and where it leads, and `open` takes the row and answers an `Opened`: a
`status`, and a `std::unique_ptr<Stream>` that is set when the status is `Ok` and null under every other.

`Tcp` carries `host`, `port` and `timeout`. The host is a name, a v4 literal or a v6 literal, and an empty
host dials the loopback. A name resolves to every address it has, and the addresses are tried in the
order the operating system gives them. Each attempt may last `timeout` (5000 ms unless the row says
otherwise); resolving the name is the operating system's own call, and its time is outside that bound.

```cpp
const net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = port, .timeout = 8000ms});
```

`open` answers `Refused` when nothing listens at the address, `Unreachable` when no route leads to it or
the name resolves to nothing, and `TimedOut` when an attempt outlasts the row's timeout.

A `Stream` is four calls, the same under every row:

| Call | Answers |
|---|---|
| `send(bytes)` | a `Transfer`: how far it got and how many bytes moved |
| `receive(into)` | a `Transfer`: how far it got and how many bytes were copied into `into` |
| `waitReadable(timeout)` | `Ok` when there is something to read, `TimedOut` for a quiet stream |
| `closed()` | `true` once the peer is gone |

A closed peer is a state to observe. A read after the peer is gone answers `Closed`, and so does every read
after it, and `closed()` keeps answering `true`. A closed peer counts as readable, so `waitReadable` returns
and the read that follows answers `Closed`.

A TCP stream sends each write as it is made: the operating system does not hold a small write back to
join it to the next one.

## Listening

`listen(at, backlog)` binds a TCP port and begins taking connections on it, and answers a `Listening`: a
`status`, and a `Listener` that holds the port when the status is `Ok`. Port 0 asks the operating system
for an unused port, and `port()` reads back the one it chose. `backlog` — 16 unless the call says
otherwise — is how many connections may wait to be accepted.

```cpp
net::Listening listening = net::listen({.port = 0});
```

`accept()` takes the next connection waiting and answers an `Opened`, the same value `open` answers;
`WouldBlock` means nobody is waiting. `waitReadable(timeout)` waits for a connection, and `TimedOut` is
the ordinary answer for a quiet port.

```cpp
net::Opened accepted = listening.listener.accept();
```

An empty host listens on every interface's IPv4 address, `0.0.0.0`, so one call binds the same family on
every operating system. `::` names every interface's IPv6 address.

A listener owns its port. Moving one hands the port over and leaves the source holding nothing, and
destroying one stops the listening. A listener holding nothing answers `Closed` from `accept` and
`waitReadable`, and `0` from `port()`.

```cpp
net::Listener moved(std::move(listening.listener));
```

## The in-process pair

`InProcess{.pair = name}` is one end of a named pair inside this process. The first `open` of a name and
the second are joined; a third answers `Refused`, because a pair has two ends. An end whose name nobody
else opens is a working stream with nobody on the far side: its reads answer `WouldBlock`, the answer a
quiet socket gives. Once both ends of a name are gone, the name opens a fresh pair.

```cpp
net::Opened first  = net::open(net::InProcess{.pair = "net connection pair"});
net::Opened second = net::open(net::InProcess{.pair = "net connection pair"});
```

The stream it answers is a `Stream` like any other, so code written against `Stream` runs over either row
unchanged. This is how a protocol is tested in one process with no network: the same function, handed a
pair of TCP streams or a pair of in-process ones, sends and reads the same bytes.

```cpp
std::vector<std::byte> exchange(net::Stream& from, net::Stream& to) {
    const std::vector<std::byte> sent = risingPattern(37);
    const std::span<const std::byte> all{sent};
    if (!sendAll(from, all.subspan(0, 12)) || !sendAll(from, all.subspan(12, 12)) ||
        !sendAll(from, all.subspan(24))) {
        return {};
    }
    return readExactly(to, sent.size(), 5);
}
```

## What a call answers

Every operating-system code the platform can tell apart maps onto one `Status`, so a caller reads no
`errno` and sees no WSA constant.

| `Status` | Means | Answered by |
|---|---|---|
| `Ok` | the call did what it was asked | every call |
| `WouldBlock` | nothing to read, nobody waiting, the send window full, or a handshake still owed bytes — the call returned rather than wait | `Stream::send` / `receive`, `Listener::accept`, `DatagramSocket::send` / `receive`, `TlsStream::handshake` |
| `Closed` | the peer is gone, and stays gone; or the listener, the socket or the stream handed over holds nothing | `Stream::send` / `receive`, every `Listener` and `DatagramSocket` call on one holding nothing, `tlsClient` / `tlsServer` handed no stream |
| `Refused` | the address is reachable and nothing listens on it; or both ends of an in-process pair are taken | `open` |
| `Unreachable` | no route to the address, or a name that resolves to nothing | `open`, `DatagramSocket::send` |
| `TimedOut` | the wait ran out | `open` (an attempt, or the handshake), every `waitReadable` |
| `Other` | anything else the operating system answered | any call |
| `Truncated` | a datagram larger than the buffer offered it: what fit was copied, and the rest is gone | `DatagramSocket::receive` |
| `Untrusted` | the peer's certificate is not one this side accepts — a verdict, the same on every later call | `TlsStream::handshake`, `open` with a `TlsClientConfig` |

## Datagrams

`bindDatagram(at)` binds a UDP port and answers a `Bound`: a `status`, and a `DatagramSocket` that holds
the port when the status is `Ok`. Port 0 asks the operating system for an unused port, and `port()` reads
it back. An empty host binds every interface's IPv4 address, as `listen` does.

```cpp
net::Bound sender   = net::bindDatagram({.host = "127.0.0.1", .port = 0});
net::Bound receiver = net::bindDatagram({.host = "127.0.0.1", .port = 0});
```

A datagram socket moves one datagram per call in both directions. `send(to, bytes)` sends `bytes` as one
datagram to the endpoint named, resolving it on that call. `receive(into)` fills `into` from exactly one
datagram and answers a `Received`: `status`, `bytes`, and `from`, the endpoint that sent it. Two datagrams
never join into one read.

```cpp
const net::Endpoint to{.host = "127.0.0.1", .port = receiver.socket.port()};
const net::Transfer first  = sender.socket.send(to, shortOne);
```

```cpp
const net::Received heard = receiver.socket.receive(into);
```

`receive` answers `WouldBlock` when no datagram is waiting, and `Truncated` when the datagram is larger
than `into`: what fit is copied, `bytes` is the size of `into`, and the rest of that datagram is gone — a
datagram is delivered once. `waitReadable(timeout)` waits for one to arrive.

A datagram socket may send to a broadcast address. It owns its port the way a listener does, and one
holding nothing answers `Closed`.

## TLS

TLS takes a `Stream` and is a `Stream`, so it runs over a TCP stream and equally over the in-process pair.
The library is the operating system's own: SecureTransport on macOS, SChannel on Windows, the system's
OpenSSL on Linux.

A client validates what its peer presents, in one of two ways, chosen by `TlsClientConfig::pinned`:

- **Nothing pinned** — the system's trust roots decide, and the certificate must carry
  `TlsClientConfig::hostname`. This is how a client reaches a public server.
- **Certificates pinned** — the peer must present one of them, its bytes equal to one pinned. This is how a
  client accepts a certificate no public authority signed, such as one a game ships for its own server.

The host name also travels in the handshake, so a server that holds several names presents the right
certificate.

The shortest form dials and brings TLS up in one call. `open(row, config)` answers a stream whose handshake
has finished, or `Untrusted` when the peer's certificate is not one `config` accepts. An empty `hostname`
takes the row's host. The handshake may last the row's `timeout`, as the connection attempt may.

```cpp
net::Opened opened = net::open(net::Tcp{.host = "127.0.0.1", .port = port, .timeout = kSettle},
                               net::TlsClientConfig{.hostname = "localhost", .pinned = {pinned}});
```

Over any other stream, `tlsClient(stream, config)` and `tlsServer(stream, config)` take the stream and
answer a `Secured`: a `status`, and a `std::unique_ptr<TlsStream>` that owns the stream it runs over. Its
handshake has not run. `handshake(timeout)` carries it as far as the bytes available allow, and answers
`WouldBlock` while the peer still owes bytes, `Ok` once settled (and on every call after), and `Untrusted`
when the peer's certificate is refused. A zero timeout makes what progress it can and returns, which is
how two ends in one thread are driven: alternate between them until both settle. A `send` or `receive`
before the handshake settles answers `WouldBlock` and moves nothing.

```cpp
net::Secured client = net::tlsClient(std::move(first),
                                     net::TlsClientConfig{.hostname = "localhost", .pinned = {pinned}});
net::Secured server = net::tlsServer(std::move(second),
                                     net::TlsServerConfig{.identity = identity, .password = kPassword});
```

A server takes TLS on a stream it accepted and drives the handshake itself, so a slow client holds only its
own stream. `TlsServerConfig::identity` is a PKCS#12 container holding the certificate and its private key,
and `password` opens it. On macOS the server side needs macOS 15 or later, where an identity is imported
without a keychain; on an earlier release `tlsServer` answers `Other`.

```cpp
net::Opened accepted = listener.accept();
```

```cpp
net::Secured secured = net::tlsServer(std::move(accepted.stream),
                                      net::TlsServerConfig{.identity = identity, .password = kPassword});
```

```cpp
const auto deadline = std::chrono::steady_clock::now() + kLiveness;
net::Status step    = net::Status::WouldBlock;
while (step == net::Status::WouldBlock && std::chrono::steady_clock::now() < deadline) {
    step = secured.stream->handshake(100ms);
}
```

A binary that names TLS records the operating system's TLS library, and a binary that does not records
none. **On Linux that library is the system's OpenSSL, which the player's machine must have at run time.**

## Addresses

`parseEndpoint(text)` reads `host:port` into an `Endpoint`:

| Text | `host` | `port` |
|---|---|---|
| `192.168.1.42:7654` | `192.168.1.42` | 7654 |
| `example.com:443` | `example.com` | 443 |
| `[::1]:7654` | `::1` | 7654 |
| `[fe80::1%en0]:80` | `fe80::1%en0` | 80 |
| `:7654` | empty | 7654 |

A v6 literal is written in brackets, because its own colons would otherwise read as the port's. The text
is read as written and no name is looked up: a name in the host is resolved where the endpoint is used.
`parseEndpoint` answers an empty `std::optional` when the text has no port, when the port is not a number
from 0 to 65535, or when a v6 literal is outside brackets.

```cpp
const std::optional<net::Endpoint> peer = net::parseEndpoint("192.168.1.42:7654");
```

`localAddresses()` answers every address this machine holds on an interface that is up, one `Interface`
per address, in the order the operating system lists them. An interface with several addresses appears
once for each.

| `Interface` field | Holds |
|---|---|
| `name` | the operating system's name for the interface: `en0`, `eth0`, `Ethernet` |
| `address` | a v4 or v6 literal |
| `prefixLength` | how many leading bits name the network: 24 for a netmask of `255.255.255.0` |
| `loopback` | `true` for an address that reaches this machine only |

The loopback is in the list — `127.0.0.1`, with a prefix of 8 — and the addresses another machine dials
are the ones with `loopback` false.

```cpp
const std::vector<net::Interface> addresses = net::localAddresses();
```

`broadcastAddress(iface)` is the address that reaches every machine on an interface's network: the
interface's address with every bit past its prefix set. It is arithmetic on the two fields and asks the
operating system nothing. It answers an empty `std::optional` for a v6 address, which has no broadcast,
and for anything that is not a v4 literal with a prefix from 0 to 32.

```cpp
const net::Interface             iface{.name = "en0", .address = "192.168.1.42", .prefixLength = 24};
const std::optional<std::string> everyone = net::broadcastAddress(iface);
```

`everyone` holds `"192.168.1.255"`. A [datagram socket](#datagrams) sends to it like any other address.

## Waiting between attempts

`backoffDelay(attempt, policy)` answers how long to wait after attempt number `attempt` — 0 for the first
— before trying again. The wait is `policy.base` doubled once per attempt and held at `policy.max`, then
moved by the jitter to a point within `jitter` of its length on either side.

| `Backoff` field | Default | Meaning |
|---|---|---|
| `base` | 250 ms | the wait after the first attempt |
| `max` | 30000 ms | the longest wait, jitter included |
| `jitter` | 0.2 | how far a wait may fall either side of its length, as a fraction of it; held between 0 and 1 |
| `seed` | 0 | what the jitter is drawn from |

```cpp
const net::Backoff steady{.jitter = 0.0f};
```

With no jitter, attempts 0, 1, 2 and 3 wait 250, 500, 1000 and 2000 ms, and every attempt from 7 on waits
30000 ms. An attempt below 0 reads as 0.

The function is pure: the same arguments answer the same wait, on every operating system. The count of
attempts and the seed are the caller's to keep. A game counts the attempts that failed and starts again
from 0 after one succeeds, and two machines that seed differently spread their retries apart rather than
retrying in step.

```cpp
const net::Backoff              policy{.seed = seed};
const std::chrono::milliseconds wait = net::backoffDelay(attempt, policy);
```

## Related pages & where to change things

- [build-and-consume.md](build-and-consume.md) — linking the platform, which carries the networking
  surface with it.
- [environment-setup.md](environment-setup.md) — the OpenSSL development headers a Linux build needs.
- [run-loop-and-timing.md](run-loop-and-timing.md) — the tick a game polls its streams and sockets from.
- The public header is `include/retropp/net.h`. The implementation is `src/net/`: the socket calls per
  operating system in `socket_posix.cpp` and `socket_win32.cpp`, which also answer `localAddresses`; the
  in-process pair in `in_process.cpp`; the streams, listeners and datagram sockets in `connection.cpp`;
  dialing with TLS in `open_tls.cpp`; TLS per operating system in `tls_securetransport.cpp`,
  `tls_schannel.cpp` and `tls_openssl.cpp`; and `parseEndpoint`, `broadcastAddress` and `backoffDelay`,
  which ask the operating system nothing, in `address.cpp`.
