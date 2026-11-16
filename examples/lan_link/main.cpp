// LAN link — two copies of one program find each other on the local network and link, and each player's
// square moves on both screens.
//
// Each copy listens for a stream on a port the OS picks, and binds a datagram socket on the discovery port
// both copies know. While no link is up it announces itself every 30 ticks — its listening port and a tag
// drawn at start — to the broadcast address of every network this machine is on and to this machine, and
// it reads the announces it hears. Of two copies that hear each other, the one with the lower tag dials the
// other. Once the link is up, every tick sends this player's position and reads the peer's; when the link
// goes down, the copy searches again.
//
// Run one copy on each of two machines on one network, or two copies on one machine. The second copy on a
// machine finds the discovery port taken, binds another, and says in its announce that it hears none, so
// the first copy dials it. On a network that drops broadcasts, start one copy with --join and the address
// and port the other copy shows. A dial happens inside a tick and waits at most one second, and the dials
// that fail are spaced by backoffDelay.
//
// Keys:
//   ARROWS      move your square
//
// Modes:
//   (no args)          the window; finds a peer by broadcast
//   --join host:port   the window; dials that address, and neither announces nor reads announces
//   --verify           no window: runs the link over the in-process pair and over a loopback socket, finds
//                      a peer from the sender a datagram names and decides which of two copies dials,
//                      sends to the broadcast address, reads a closed peer as closed, and reads --join's
//                      argument. One line per check; exits nonzero on any miss (CI runs this on every
//                      platform).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/asset_policy.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/net.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;
using namespace std::chrono_literals;
namespace net = retropp::net;

// ── The announce ──────────────────────────────────────────────────────────────────────────────────
// The datagram a copy sends to be found: the eight characters RPPLINK1, the port its listener holds (2
// bytes), its tag (4 bytes), and whether its datagram socket hears announces (1 byte) — 15 bytes, each
// number least significant byte first. The bytes are this program's own; any format two copies agree on
// does the same job.
constexpr std::uint16_t kDiscoveryPort = 47624;
constexpr std::size_t   kAnnounceSize  = 15;
constexpr std::array<char, 8> kAnnounceHeader{'R', 'P', 'P', 'L', 'I', 'N', 'K', '1'};

struct Announce {
    std::uint16_t port = 0;
    std::uint32_t tag  = 0;
    bool          deaf = false;  // its datagram socket is not on the discovery port, so it hears no announce
};

[[nodiscard]] std::array<std::byte, kAnnounceSize> writeAnnounce(const Announce& announce) {
    std::array<std::byte, kAnnounceSize> bytes{};
    for (std::size_t i = 0; i < kAnnounceHeader.size(); ++i) {
        bytes[i] = static_cast<std::byte>(kAnnounceHeader[i]);
    }
    bytes[8] = static_cast<std::byte>(announce.port & 0xFF);
    bytes[9] = static_cast<std::byte>(announce.port >> 8);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[10 + i] = static_cast<std::byte>((announce.tag >> (8 * i)) & 0xFF);
    }
    bytes[14] = static_cast<std::byte>(announce.deaf ? 1 : 0);
    return bytes;
}

// Nothing for bytes of the wrong length or with another header.
[[nodiscard]] std::optional<Announce> readAnnounce(std::span<const std::byte> bytes) {
    if (bytes.size() != kAnnounceSize) return std::nullopt;
    for (std::size_t i = 0; i < kAnnounceHeader.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(kAnnounceHeader[i])) return std::nullopt;
    }
    const auto at = [&](std::size_t i) { return std::to_integer<std::uint32_t>(bytes[i]); };
    Announce announce;
    announce.port = static_cast<std::uint16_t>(at(8) | (at(9) << 8));
    announce.tag  = at(10) | (at(11) << 8) | (at(12) << 16) | (at(13) << 24);
    announce.deaf = at(14) != 0;
    return announce;
}

// Send this copy's announce to every network this machine is on, and to this machine.
void announce(net::DatagramSocket& discovery, const Announce& self) {
    const std::array<std::byte, kAnnounceSize> bytes = writeAnnounce(self);
    for (const net::Interface& iface : net::localAddresses()) {
        if (iface.loopback) continue;
        if (const std::optional<std::string> everyone = net::broadcastAddress(iface)) {
            (void)discovery.send({.host = *everyone, .port = kDiscoveryPort}, bytes);
        }
    }
    (void)discovery.send({.host = "127.0.0.1", .port = kDiscoveryPort}, bytes);
}

// Read every announce waiting. Answers the copy this one should dial, when one was heard: one that hears no
// announces, or one whose tag is greater than this copy's. The copy with the greater tag waits to be
// dialed.
[[nodiscard]] std::optional<net::Endpoint> hear(net::DatagramSocket& discovery, const Announce& self) {
    std::optional<net::Endpoint> target;
    std::array<std::byte, 64>    buffer{};
    for (;;) {
        const net::Received heard = discovery.receive(buffer);
        if (heard.status == net::Status::Truncated) continue;
        if (heard.status != net::Status::Ok) return target;

        const std::optional<Announce> other =
            readAnnounce(std::span<const std::byte>(buffer).first(heard.bytes));
        if (!other || other->tag == self.tag) continue;
        if (other->deaf || other->tag > self.tag) {
            target = net::Endpoint{.host = heard.from.host, .port = other->port};
        }
    }
}

// ── The link ──────────────────────────────────────────────────────────────────────────────────────
// Each step sends this player's position and reads what the peer has sent. A position is four bytes — x
// then y, two bytes each, least significant first — and the stream carries them in order, so the peer's
// position is the last whole four bytes read.
struct Position {
    int x = 0;
    int y = 0;

    bool operator==(const Position&) const = default;
};

constexpr std::size_t kPositionSize = 4;

[[nodiscard]] std::array<std::byte, kPositionSize> writePosition(Position p) {
    const auto x = static_cast<std::uint16_t>(p.x);
    const auto y = static_cast<std::uint16_t>(p.y);
    return {static_cast<std::byte>(x & 0xFF), static_cast<std::byte>(x >> 8),
            static_cast<std::byte>(y & 0xFF), static_cast<std::byte>(y >> 8)};
}

[[nodiscard]] Position readPosition(std::span<const std::byte> bytes) {
    const auto at = [&](std::size_t i) { return std::to_integer<int>(bytes[i]); };
    return Position{.x = at(0) | (at(1) << 8), .y = at(2) | (at(3) << 8)};
}

class Link {
public:
    explicit Link(std::unique_ptr<net::Stream> stream) : stream_(std::move(stream)) {}

    void step(Position mine) {
        if (!up_) return;
        send(mine);
        if (up_) receive();
    }

    // False once a send or a receive has answered Closed, and from then on.
    [[nodiscard]] bool up() const noexcept { return up_; }

    // The last position the peer sent, once one has arrived.
    [[nodiscard]] std::optional<Position> peer() const noexcept { return peer_; }

    [[nodiscard]] net::Status waitReadable(std::chrono::milliseconds timeout) {
        return stream_->waitReadable(timeout);
    }

private:
    // The rest of a position a send cut short goes first, so the peer never reads part of one. A position
    // the stream does not take this tick is dropped, and the next tick's position takes its place.
    void send(Position mine) {
        const bool fresh = owed_.empty();
        if (fresh) {
            const std::array<std::byte, kPositionSize> bytes = writePosition(mine);
            owed_.assign(bytes.begin(), bytes.end());
        }
        const net::Transfer moved = stream_->send(owed_);
        if (moved.status == net::Status::Closed) {
            up_ = false;
            return;
        }
        if (moved.status != net::Status::Ok) {
            if (fresh) owed_.clear();
            return;
        }
        owed_.erase(owed_.begin(), owed_.begin() + static_cast<std::ptrdiff_t>(moved.bytes));
    }

    void receive() {
        std::array<std::byte, 64> chunk{};
        for (;;) {
            const net::Transfer moved = stream_->receive(chunk);
            if (moved.status == net::Status::Closed) {
                up_ = false;
                return;
            }
            if (moved.status != net::Status::Ok || moved.bytes == 0) break;
            inbox_.insert(inbox_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(moved.bytes));
        }
        const std::size_t whole = inbox_.size() / kPositionSize;
        if (whole == 0) return;
        peer_ = readPosition(std::span<const std::byte>(inbox_).subspan((whole - 1) * kPositionSize, kPositionSize));
        inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(whole * kPositionSize));
    }

    std::unique_ptr<net::Stream> stream_;
    std::vector<std::byte>       owed_;
    std::vector<std::byte>       inbox_;
    std::optional<Position>      peer_;
    bool                         up_ = true;
};

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────
constexpr std::chrono::milliseconds kSettle = 5000ms;
constexpr int                       kTurns  = 64;

[[nodiscard]] Position firstAt(int i) { return Position{.x = i * 7 + 1, .y = i * 3 + 2}; }
[[nodiscard]] Position secondAt(int i) { return Position{.x = i * 5 + 3, .y = i * 11 + 4}; }

[[nodiscard]] const char* statusName(net::Status status) {
    switch (status) {
        case net::Status::Ok:          return "Ok";
        case net::Status::WouldBlock:  return "WouldBlock";
        case net::Status::Closed:      return "Closed";
        case net::Status::Refused:     return "Refused";
        case net::Status::Unreachable: return "Unreachable";
        case net::Status::TimedOut:    return "TimedOut";
        case net::Status::Other:       return "Other";
        case net::Status::Truncated:   return "Truncated";
        case net::Status::Untrusted:   return "Untrusted";
    }
    return "?";
}

[[nodiscard]] std::string said(std::optional<Position> p) {
    return p ? std::to_string(p->x) + "," + std::to_string(p->y) : std::string("nothing");
}

// Check 1. Two links over the in-process pair step unevenly for 64 turns: in turn i the first steps
// 1 + i % 3 times, then the second 1 + (i + 1) % 3 times, each step sending that side's next position. A
// side's first step of a turn reads everything the other sent in its last run of steps — one, two or three
// positions — and the side holds the last of them. Empty when every turn reads right; otherwise the first
// miss.
[[nodiscard]] std::string overTheInProcessPair() {
    net::Opened a = net::open(net::InProcess{.pair = "lan link verify"});
    net::Opened b = net::open(net::InProcess{.pair = "lan link verify"});
    if (a.status != net::Status::Ok || b.status != net::Status::Ok) {
        return std::string("open answered ") + statusName(a.status) + " and " + statusName(b.status);
    }
    Link first(std::move(a.stream));
    Link second(std::move(b.stream));

    int                     firstSent  = 0;
    int                     secondSent = 0;
    std::optional<Position> secondLast;  // the last position the second sent, as of the turn before
    for (int i = 0; i < kTurns; ++i) {
        for (int s = 0; s < 1 + i % 3; ++s) first.step(firstAt(firstSent++));
        if (first.peer() != secondLast) {
            return "turn " + std::to_string(i) + ": the first holds " + said(first.peer()) + ", want " +
                   said(secondLast);
        }

        for (int s = 0; s < 1 + (i + 1) % 3; ++s) second.step(secondAt(secondSent++));
        const Position firstLast = firstAt(firstSent - 1);
        if (second.peer() != firstLast) {
            return "turn " + std::to_string(i) + ": the second holds " + said(second.peer()) + ", want " +
                   said(firstLast);
        }
        secondLast = secondAt(secondSent - 1);
    }
    if (!first.up() || !second.up()) return "a link went down";
    return {};
}

// Check 2. Two links step in turn, the first then the second, for 64 turns, each read preceded by a wait
// for the bytes so a socket's delivery is never raced. After each turn the second holds the first's
// position from this turn, and the first holds the second's from the turn before — the latest each had
// sent when the other read. Empty when every turn reads right; otherwise the first miss.
[[nodiscard]] std::string stepInTurn(Link& first, Link& second) {
    for (int i = 0; i < kTurns; ++i) {
        if (i > 0) (void)first.waitReadable(kSettle);
        first.step(firstAt(i));
        (void)second.waitReadable(kSettle);
        second.step(secondAt(i));

        const std::optional<Position> firstWants =
            i == 0 ? std::nullopt : std::optional<Position>(secondAt(i - 1));
        if (second.peer() != firstAt(i)) {
            return "turn " + std::to_string(i) + ": the second holds " + said(second.peer()) + ", want " +
                   said(firstAt(i));
        }
        if (first.peer() != firstWants) {
            return "turn " + std::to_string(i) + ": the first holds " + said(first.peer()) + ", want " +
                   said(firstWants);
        }
    }
    if (!first.up() || !second.up()) return "a link went down";
    return {};
}

// The two ends of a loopback stream, as links.
struct LinkedPair {
    Link first;
    Link second;
};

[[nodiscard]] std::optional<LinkedPair> overLoopback(std::string& why) {
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    if (listening.status != net::Status::Ok) {
        why = std::string("listen answered ") + statusName(listening.status);
        return std::nullopt;
    }
    net::Opened dialed = net::open(net::Tcp{.host = "127.0.0.1", .port = listening.listener.port()});
    if (dialed.status != net::Status::Ok) {
        why = std::string("open answered ") + statusName(dialed.status);
        return std::nullopt;
    }
    (void)listening.listener.waitReadable(kSettle);
    net::Opened accepted = listening.listener.accept();
    if (accepted.status != net::Status::Ok) {
        why = std::string("accept answered ") + statusName(accepted.status);
        return std::nullopt;
    }
    return LinkedPair{.first = Link(std::move(dialed.stream)), .second = Link(std::move(accepted.stream))};
}

// Check 3.
[[nodiscard]] std::string aPeerFoundFromItsSender() {
    net::Bound     announcer = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    net::Bound     hearer    = net::bindDatagram({.host = "127.0.0.1", .port = 0});
    net::Listening listening = net::listen({.host = "127.0.0.1", .port = 0});
    if (announcer.status != net::Status::Ok || hearer.status != net::Status::Ok ||
        listening.status != net::Status::Ok) {
        return "a socket or the listener did not bind";
    }
    const net::Endpoint to{.host = "127.0.0.1", .port = hearer.socket.port()};
    const Announce      sent{.port = listening.listener.port(), .tag = 7, .deaf = false};
    std::array<std::byte, 64> buffer{};

    const net::Transfer put = announcer.socket.send(to, writeAnnounce(sent));
    if (put.status != net::Status::Ok) return std::string("the announce's send answered ") + statusName(put.status);
    (void)hearer.socket.waitReadable(kSettle);
    const net::Received heard = hearer.socket.receive(buffer);
    if (heard.status != net::Status::Ok || heard.bytes != kAnnounceSize) {
        return std::string("the receive answered ") + statusName(heard.status) + " with " +
               std::to_string(heard.bytes) + " bytes";
    }
    if (heard.from.host != "127.0.0.1" || heard.from.port != announcer.socket.port()) {
        return "the sender reads as " + heard.from.host + ":" + std::to_string(heard.from.port);
    }
    const std::optional<Announce> announce = readAnnounce(std::span<const std::byte>(buffer).first(heard.bytes));
    if (!announce || announce->port != sent.port || announce->tag != sent.tag || announce->deaf != sent.deaf) {
        return "the announce did not read back as it was written";
    }
    const net::Opened link = net::open(net::Tcp{.host = heard.from.host, .port = announce->port});
    if (link.status != net::Status::Ok) {
        return std::string("dialing the sender answered ") + statusName(link.status);
    }

    // The same bytes under another header are a datagram like any other, and not an announce.
    std::array<std::byte, kAnnounceSize> other = writeAnnounce(sent);
    other[7] = static_cast<std::byte>('2');
    const net::Transfer putOther = announcer.socket.send(to, other);
    (void)hearer.socket.waitReadable(kSettle);
    const net::Received heardOther = hearer.socket.receive(buffer);
    if (putOther.status != net::Status::Ok || heardOther.status != net::Status::Ok ||
        heardOther.bytes != kAnnounceSize) {
        return "the RPPLINK2 datagram did not arrive whole";
    }
    if (readAnnounce(std::span<const std::byte>(buffer).first(heardOther.bytes))) {
        return "an RPPLINK2 datagram read as an announce";
    }

    // Which of two copies dials: this copy, tag 5, dials a copy with a greater tag and one that hears no
    // announces, and waits to be dialed by a copy with a lower tag.
    struct Row {
        Announce    sent;
        bool        dials;
        bool        opens;  // the dial is made too
        const char* what;
    };
    const Announce self{.port = 0, .tag = 5, .deaf = false};
    const std::array<Row, 4> rows{{
        {.sent = {.port = sent.port, .tag = 7, .deaf = false}, .dials = true, .opens = true, .what = "tag 7"},
        {.sent = {.port = sent.port, .tag = 3, .deaf = false}, .dials = false, .opens = false, .what = "tag 3"},
        {.sent = {.port = sent.port, .tag = 3, .deaf = true}, .dials = true, .opens = false, .what = "tag 3, deaf"},
        {.sent  = {.port = sent.port, .tag = 5, .deaf = false},
         .dials = false,
         .opens = false,
         .what  = "this copy's own tag"},
    }};
    for (const Row& row : rows) {
        if (announcer.socket.send(to, writeAnnounce(row.sent)).status != net::Status::Ok) {
            return std::string("the announce for ") + row.what + " did not send";
        }
        (void)hearer.socket.waitReadable(kSettle);
        const std::optional<net::Endpoint> target = hear(hearer.socket, self);
        if (!row.dials) {
            if (target) return std::string("hear named a copy to dial for ") + row.what;
            continue;
        }
        if (!target || target->host != "127.0.0.1" || target->port != sent.port) {
            return std::string("hear did not name the copy that announced ") + row.what;
        }
        if (row.opens) {
            const net::Opened dialed = net::open(net::Tcp{.host = target->host, .port = target->port});
            if (dialed.status != net::Status::Ok) {
                return std::string("dialing what hear named answered ") + statusName(dialed.status);
            }
        }
    }
    return {};
}

// Check 4. A machine with no network answers Unreachable.
[[nodiscard]] std::string aBroadcastSendIsAccepted() {
    net::Bound socket = net::bindDatagram({.port = 0});
    if (socket.status != net::Status::Ok) return std::string("bindDatagram answered ") + statusName(socket.status);
    const net::Transfer sent = socket.socket.send({.host = "255.255.255.255", .port = kDiscoveryPort},
                                                  writeAnnounce({.port = 1, .tag = 7, .deaf = false}));
    if (sent.status != net::Status::Ok && sent.status != net::Status::Unreachable) {
        return std::string("the send answered ") + statusName(sent.status);
    }
    return {};
}

// Check 5. The first end of a loopback link is destroyed; the second reads it as closed within 100 steps,
// and stays down.
[[nodiscard]] std::string aClosedPeerReadsAsClosed(std::optional<LinkedPair>& pair) {
    if (!pair) return "no loopback link to close";
    Link second = std::move(pair->second);
    pair.reset();
    for (int i = 0; i < 100 && second.up(); ++i) {
        (void)second.waitReadable(100ms);
        second.step(secondAt(i));
    }
    if (second.up()) return "the link is up after 100 steps";
    second.step(secondAt(0));
    if (second.up()) return "the link came back up";
    return {};
}

// Check 6.
[[nodiscard]] std::string joinReadsHostAndPort() {
    const std::optional<net::Endpoint> joined = net::parseEndpoint("192.168.1.42:49152");
    if (!joined || joined->host != "192.168.1.42" || joined->port != 49152) {
        return "192.168.1.42:49152 did not read as that host and port";
    }
    if (net::parseEndpoint("192.168.1.42")) return "192.168.1.42 read as an endpoint";
    return {};
}

int runVerify() {
    int        failures = 0;
    const auto report   = [&](int number, std::string_view check, const std::string& miss) {
        if (miss.empty()) {
            std::printf("lan link verify: %d %.*s: PASS\n", number, static_cast<int>(check.size()), check.data());
            return;
        }
        std::printf("lan link verify: %d %.*s: FAIL — %s\n", number, static_cast<int>(check.size()), check.data(),
                    miss.c_str());
        ++failures;
    };

    report(1, "the link over the in-process pair", overTheInProcessPair());

    std::string                why;
    std::optional<LinkedPair>  loopback = overLoopback(why);
    report(2, "the link over a loopback socket",
           loopback ? stepInTurn(loopback->first, loopback->second) : why);

    report(3, "a peer found from the sender a datagram names", aPeerFoundFromItsSender());
    report(4, "a send to the broadcast address is accepted", aBroadcastSendIsAccepted());
    report(5, "a closed peer reads as closed", aClosedPeerReadsAsClosed(loopback));
    report(6, "parseEndpoint reads what --join is given", joinReadsHostAndPort());

    std::printf("lan link verify: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// ── The window ────────────────────────────────────────────────────────────────────────────────────
constexpr int kViewW   = 640;
constexpr int kViewH   = 360;
constexpr int kCell    = 8;
constexpr int kMapW    = kViewW / kCell;
constexpr int kMapH    = kViewH / kCell;
constexpr int kSquare  = 24;
constexpr int kSpeed   = 4;   // pixels a tick
constexpr int kPlayTop = 72;  // the squares stay below the status lines
constexpr int kPlayBottom    = kViewH - 40;
constexpr int kAnnounceEvery = 30;  // ticks between announces while no link is up
constexpr int kGlyphPx       = 16;

// One 32×32 sheet: index 1 over its top-left 24×24, index 0 elsewhere. A square is a 24×24 sprite at cell
// 0, and the backdrop draws cell 0, all index 1, through a palette whose two entries are its one color.
struct Scene {
    AtlasId   sheet{};
    PaletteId backdrop{};
    PaletteId mine{};
    PaletteId peer{};
    std::vector<TileCell> cells;
};

[[nodiscard]] Scene uploadScene(Renderer& r) {
    constexpr int             kSheet = 32;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kSheet) * kSheet, 0);
    for (int y = 0; y < kSquare; ++y)
        for (int x = 0; x < kSquare; ++x) px[static_cast<std::size_t>(y) * kSheet + static_cast<std::size_t>(x)] = 1;

    const auto two = [&](Rgba8 color) {
        const std::array<Rgba8, 2> entries{{color, color}};
        return r.uploadPalette(std::span<const Rgba8>(entries));
    };
    Scene scene;
    scene.sheet    = r.uploadAtlas(px.data(), kSheet, kSheet).atlasId;
    scene.backdrop = two(Rgba8{.r = 16, .g = 20, .b = 32});
    scene.mine     = two(Rgba8{.r = 80, .g = 210, .b = 200});
    scene.peer     = two(Rgba8{.r = 250, .g = 150, .b = 60});
    scene.cells.assign(static_cast<std::size_t>(kMapW) * kMapH,
                       TileCell{.atlas = scene.sheet, .tile = 0, .palette = scene.backdrop});
    return scene;
}

[[nodiscard]] DrawLayer backdropLayer(const Scene& scene) {
    DrawLayer layer{.key = "backdrop"};
    layer.z       = 0;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.content = TileContent{.widthInTiles  = kMapW,
                                .heightInTiles = kMapH,
                                .cells         = std::span<const TileCell>(scene.cells),
                                .wrap          = TileWrap::Blank};
    return layer;
}

[[nodiscard]] Sprite square(std::string key, Position at, const Scene& scene, PaletteId palette) {
    return Sprite{.key     = std::move(key),
                  .x       = at.x,
                  .y       = at.y,
                  .size    = AssetDimensions{.width = kSquare, .height = kSquare},
                  .atlas   = scene.sheet,
                  .tile    = 0,
                  .palette = palette};
}

struct Fonts {
    AtlasManifest font;
    PaletteId     text;
    PaletteId     dim;
};

// The font's cell for a character. Letters draw as capitals, so a host name typed in lowercase reads in the
// font's one case.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    switch (ch) {
        case '.': return 38;
        case '-': return 39;
        default:  return 36;  // a space, and any character the font has no glyph for
    }
}

void write(std::vector<Sprite>& out, const Fonts& fonts, int y, std::string_view text, PaletteId palette,
           std::string_view keyPrefix) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (glyphCell(text[i]) == 36) continue;
        out.push_back(Sprite{.key     = std::string(keyPrefix) + std::to_string(i),
                             .x       = 16 + static_cast<int>(i) * kGlyphPx,
                             .y       = y,
                             .size    = AssetDimensions{.width = kGlyphPx, .height = kGlyphPx},
                             .atlas   = fonts.font.atlasId,
                             .tile    = static_cast<std::uint16_t>(fonts.font[glyphCell(text[i])].tile),
                             .palette = palette});
    }
}

// The first v4 address another machine on the network dials to reach this one.
[[nodiscard]] std::optional<std::string> networkAddress() {
    for (const net::Interface& iface : net::localAddresses()) {
        if (!iface.loopback && iface.address.find(':') == std::string::npos) return iface.address;
    }
    return std::nullopt;
}

enum class Action : std::uint8_t { Left, Right, Up, Down };

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }
    std::optional<net::Endpoint> join;
    if (argc > 1 && std::strcmp(argv[1], "--join") == 0) {
        if (argc > 2) join = net::parseEndpoint(argv[2]);
        if (!join) {
            std::fprintf(stderr, "lan link: --join takes the other copy's host:port, as 192.168.1.42:49152\n");
            return 2;
        }
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "LanLink"},
        .window   = {.title = "Polyrhythm — LAN link"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Left, {SDL_SCANCODE_LEFT}},
                  {Action::Right, {SDL_SCANCODE_RIGHT}},
                  {Action::Up, {SDL_SCANCODE_UP}},
                  {Action::Down, {SDL_SCANCODE_DOWN}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font = renderer.loadAtlas("examples/lan_link/assets/art/font.png", AssetDimensions{.width = 16, .height = 16},
                                    ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64, TransparentIndices::None,
                                    0, AssetPolicy::Embed);
    fonts.text = renderer.loadPaletteImage("examples/lan_link/assets/palettes/font.png", ReadOrder::LeftRightThenDown,
                                           0, AssetPolicy::Embed);
    fonts.dim  = renderer.loadPaletteImage("examples/lan_link/assets/palettes/mono.png", ReadOrder::LeftRightThenDown,
                                           0, AssetPolicy::Embed);
    const Scene scene = uploadScene(renderer);

    // This copy's listener, its discovery socket and its announce.
    net::Listening listening = net::listen({.port = 0});
    if (listening.status != net::Status::Ok) {
        std::fprintf(stderr, "lan link: listening on a port failed\n");
        return 1;
    }
    Announce   self{.port = listening.listener.port(), .tag = std::random_device{}(), .deaf = false};
    net::Bound discovery = net::bindDatagram({.port = kDiscoveryPort});
    if (discovery.status != net::Status::Ok) {
        discovery = net::bindDatagram({.port = 0});
        self.deaf = true;
    }

    std::optional<Link>                   link;
    std::string                           linkedTo;  // the host this copy dialed; empty when it accepted
    std::optional<std::string>            address     = networkAddress();
    int                                   searchTicks = 0;
    int                                   attempt     = 0;  // dials that failed since the last link
    std::chrono::steady_clock::time_point notBefore{};      // the next dial waits until then
    Position mine{.x = 40 + static_cast<int>(self.tag % (kViewW - 80 - kSquare)),
                  .y = kPlayTop + static_cast<int>((self.tag >> 16) % (kPlayBottom - kPlayTop - kSquare))};

    loop.simTick([&](const InputState& in) {
        const int dx = (in.isHeld(Action::Right) ? kSpeed : 0) - (in.isHeld(Action::Left) ? kSpeed : 0);
        const int dy = (in.isHeld(Action::Down) ? kSpeed : 0) - (in.isHeld(Action::Up) ? kSpeed : 0);
        mine.x       = std::clamp(mine.x + dx, 0, kViewW - kSquare);
        mine.y       = std::clamp(mine.y + dy, kPlayTop, kPlayBottom - kSquare);

        if (link) {
            link->step(mine);
            if (!link->up()) {
                link.reset();
                linkedTo.clear();
                searchTicks = 0;
                notBefore   = {};
            }
            return;
        }

        if (searchTicks++ % kAnnounceEvery == 0) {
            address = networkAddress();
            if (!join) announce(discovery.socket, self);
        }

        net::Opened accepted = listening.listener.accept();
        if (accepted.status == net::Status::Ok) {
            link.emplace(std::move(accepted.stream));
            attempt = 0;
            return;
        }

        // One place dials, for the address --join names and for a copy heard announcing. A dial that fails
        // waits backoffDelay before the next.
        const std::optional<net::Endpoint> target = join ? join : hear(discovery.socket, self);
        if (target && std::chrono::steady_clock::now() >= notBefore) {
            net::Opened opened = net::open(net::Tcp{.host = target->host, .port = target->port, .timeout = 1000ms});
            if (opened.status == net::Status::Ok) {
                link.emplace(std::move(opened.stream));
                linkedTo = target->host;
                attempt  = 0;
            } else {
                notBefore = std::chrono::steady_clock::now() + net::backoffDelay(attempt++, {.seed = self.tag});
            }
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        std::vector<Sprite> sprites;
        const std::string   status =
            link ? (linkedTo.empty() ? std::string("A PEER DIALED IN AND LINKED") : "LINKED TO " + linkedTo)
            : join ? "DIALING " + join->host + " PORT " + std::to_string(join->port)
                   : std::string("SEARCHING");
        const std::string where = "YOUR ADDRESS " + address.value_or("NONE") + " PORT " + std::to_string(self.port);
        write(sprites, fonts, 12, status, fonts.text, "status");
        write(sprites, fonts, 36, where, fonts.dim, "where");
        write(sprites, fonts, kViewH - 28, "ARROWS MOVE YOUR SQUARE", fonts.dim, "keys");

        if (link && link->peer()) sprites.push_back(square("peer", *link->peer(), scene, scene.peer));
        sprites.push_back(square("mine", mine, scene, scene.mine));

        DrawLayer top{.key = "top"};
        top.z       = 10;
        top.size    = PixelSize{kViewW, kViewH};
        top.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};

        frame.layers = {backdropLayer(scene), top};
        renderer.renderFrame(frame);
    });

    std::printf(
        "LAN link: run a second copy on another machine on this network, or a second copy here. The two find\n"
        "each other and link, and each player's square moves on both screens. ARROWS move your square. On a\n"
        "network that drops broadcasts, start one copy with --join and the address and port the other shows.\n"
        "Close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
