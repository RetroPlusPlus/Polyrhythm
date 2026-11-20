// A worker pool stepping a room of actors, beside the same room stepped inline.
//
// Both rooms start from one seed. Every actor runs its own small program: each tick a step reads a
// snapshot of what the actor sees (where it is, where the beacon is, the tick), runs WORK rounds of
// integer arithmetic on the actor's own registers, and writes one request (a step, a digit and a
// color). The requests are applied at the top of the next tick on the main thread, in index order; a
// step into a cell an earlier actor stepped into that tick is refused, so the order decides the room.
// The beacon walks a fixed path around the room and the actors chase it.
//
// The left room is stepped through WorkerPool pool{platform}: the steps are handed over at the bottom
// of the tick and run while the main thread renders and paces. The right room is stepped through
// WorkerPool inlinePool{platform, 0}, a pool with no workers, so every step runs when the batch is
// collected. ROOMS MATCH compares the two rooms' whole state every tick.
//
// The readout:
//
//   WORKERS           how many workers the pool started, of the platform's cores
//   ACTORS            the actors in each room; LEFT BRACKET halves it, RIGHT BRACKET doubles it
//   WORK              the rounds each step runs; MINUS halves it, EQUALS doubles it
//   POOL COLLECT      how long the left room's collect took last tick, and how many of the last 60
//                     ticks found the batch finished before collect was called
//   INLINE COLLECT    how long the right room's collect took last tick: the whole batch, run there
//   ROOMS MATCH       whether the two rooms hold the same state
//
// Close the window to quit.
//
// --verify runs 256 actors for 600 ticks four ways — a plain loop, WorkerPool{platform},
// WorkerPool{platform, 0} and WorkerPool{platform, 3} — and checks after every tick that each pool's
// room holds the plain loop's state. It checks WorkerPool{platform}.workers() against the count the
// platform's cores give, and prints what submit and a finished collect cost. It exits nonzero on any
// mismatch, and 77 with the reason when SDL cannot open a window.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"
#include "retropp/worker_pool.h"

#include "src/sdl_platform_testing.h"

using namespace retropp;

namespace {

constexpr int kTilePx = 8;
constexpr int kGlyphPx = 16, kGlyphStride = 128 / kTilePx;  // font.png is 128 wide
constexpr int kCols = 48, kRows = 24;
constexpr int kViewW = kCols * kGlyphPx, kViewH = kRows * kGlyphPx;

// Each room is a grid of glyph cells. The left room's first column is kLeftCol, the right room's
// kRightCol; both start at kRoomRow.
constexpr int kRoomW = 22, kRoomH = 13;
constexpr int kLeftCol = 1, kRightCol = 25, kRoomRow = 2;

constexpr std::size_t   kMinActors = 8, kMaxActors = 1024, kStartActors = 64;
constexpr int           kMinWork = 250, kMaxWork = 64000, kStartWork = 2000;
constexpr std::uint32_t kSeed = 0x5EEDu;

// Ticks the beacon stays in a cell before it takes the next one.
constexpr std::uint32_t kBeaconTicksPerCell = 8;

// The window the share of finished-before-collect batches covers.
constexpr int kShareTicks = 60;

enum class Action : std::uint8_t { FewerActors, MoreActors, LessWork, MoreWork };

struct Cell {
    int x = 0, y = 0;
};

// What one actor's step reads: the actor's cell, the beacon's cell and the tick, gathered on the main
// thread before the batch is handed over.
struct Snapshot {
    Cell          at;
    Cell          beacon;
    std::uint32_t tick = 0;
};

// What one actor's step writes: a step of -1, 0 or 1 on each axis, and the digit and color it shows.
struct Request {
    int          dx = 0, dy = 0;
    std::uint8_t digit = 0, color = 0;

    bool operator==(const Request&) const = default;
};

struct Actor {
    Cell         at;
    std::uint8_t digit = 0, color = 0;

    bool operator==(const Actor& other) const {
        return at.x == other.at.x && at.y == other.at.y && digit == other.digit && color == other.color;
    }
};

using Registers = std::array<std::uint32_t, 4>;

// A small linear congruential generator, so both rooms and every run start from the same actors.
[[nodiscard]] std::uint32_t nextSeed(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

[[nodiscard]] int signOf(int v) { return (v > 0) - (v < 0); }

// One actor's program: `work` rounds of integer mixing on its own registers, started from what it
// sees, then one request — a step toward the beacon, or a stray step when the registers say so, and
// the digit and color to show next.
void runProgram(const Snapshot& seen, Registers& reg, Request& out, int work) {
    std::uint32_t a = reg[0] ^ (seen.tick * 0x9E3779B9u);
    std::uint32_t b = reg[1] + static_cast<std::uint32_t>(seen.at.x * 31 + seen.at.y);
    std::uint32_t c = reg[2] + static_cast<std::uint32_t>(seen.beacon.x * 17 + seen.beacon.y);
    std::uint32_t d = reg[3];
    for (int round = 0; round < work; ++round) {
        a += b;
        d = std::rotl(d ^ a, 16);
        c += d;
        b = std::rotl(b ^ c, 12);
        a += b;
        d = std::rotl(d ^ a, 8);
        c += d;
        b = std::rotl(b ^ c, 7);
    }
    reg = {a, b, c, d};

    const bool strays = (a & 7u) == 0;
    out.dx    = strays ? static_cast<int>(b % 3u) - 1 : signOf(seen.beacon.x - seen.at.x);
    out.dy    = strays ? static_cast<int>(c % 3u) - 1 : signOf(seen.beacon.y - seen.at.y);
    out.digit = static_cast<std::uint8_t>(d % 10u);
    out.color = static_cast<std::uint8_t>((a >> 3) % 3u);
}

// The beacon's cell at `tick`: a walk clockwise around the room, two cells in from its edge.
[[nodiscard]] Cell beaconAt(std::uint32_t tick) {
    constexpr int kInset = 2;
    constexpr int kW = kRoomW - 2 * kInset - 1, kH = kRoomH - 2 * kInset - 1;
    const int     step = static_cast<int>((tick / kBeaconTicksPerCell) % (2 * (kW + kH)));
    if (step < kW) return {kInset + step, kInset};
    if (step < kW + kH) return {kInset + kW, kInset + (step - kW)};
    if (step < 2 * kW + kH) return {kInset + kW - (step - kW - kH), kInset + kH};
    return {kInset, kInset + kH - (step - 2 * kW - kH)};
}

// A room: its actors, and for each actor the snapshot its step reads, the registers its program owns
// and the request its step writes. A step touches only its own actor's three entries, so the room's
// state after collect is the state a plain loop gives.
struct Room {
    std::vector<Actor>     actors;
    std::vector<Snapshot>  seen;
    std::vector<Registers> registers;
    std::vector<Request>   requests;
    std::vector<bool>      steppedInto = std::vector<bool>(kRoomW * kRoomH);

    void reset(std::size_t count) {
        std::uint32_t state = kSeed;
        actors.assign(count, Actor{});
        seen.assign(count, Snapshot{});
        registers.assign(count, Registers{});
        requests.assign(count, Request{});
        for (std::size_t i = 0; i < count; ++i) {
            actors[i].at = Cell{.x = static_cast<int>(nextSeed(state) % kRoomW),
                                .y = static_cast<int>(nextSeed(state) % kRoomH)};
            for (std::uint32_t& r : registers[i]) r = nextSeed(state);
            requests[i] = Request{.digit = static_cast<std::uint8_t>(i % 10), .color = 0};
        }
    }

    // The requests, applied in index order. A step into a cell an earlier actor stepped into this
    // tick is refused; the digit and color always land.
    void apply() {
        std::fill(steppedInto.begin(), steppedInto.end(), false);
        for (std::size_t i = 0; i < actors.size(); ++i) {
            Actor&         actor = actors[i];
            const Request& r     = requests[i];
            const Cell     to{.x = std::clamp(actor.at.x + r.dx, 0, kRoomW - 1),
                              .y = std::clamp(actor.at.y + r.dy, 0, kRoomH - 1)};
            const auto     cell = static_cast<std::size_t>(to.y * kRoomW + to.x);
            if ((to.x != actor.at.x || to.y != actor.at.y) && !steppedInto[cell]) {
                actor.at          = to;
                steppedInto[cell] = true;
            }
            actor.digit = r.digit;
            actor.color = r.color;
        }
    }

    void gather(Cell beacon, std::uint32_t tick) {
        for (std::size_t i = 0; i < actors.size(); ++i) {
            seen[i] = Snapshot{.at = actors[i].at, .beacon = beacon, .tick = tick};
        }
    }

    void step(std::size_t i, int work) { runProgram(seen[i], registers[i], requests[i], work); }

    [[nodiscard]] bool sameAs(const Room& other) const {
        return actors == other.actors && registers == other.registers && requests == other.requests;
    }
};

// Hands the room's steps to `pool`. The job holds a pointer to the room and the work count.
[[nodiscard]] WorkerPool::Batch submitSteps(WorkerPool& pool, Room& room, int work) {
    Room* const r = &room;
    return pool.submit(room.actors.size(), [r, work](std::size_t i) { r->step(i, work); });
}

[[nodiscard]] EngineConfig demoConfig() {
    return EngineConfig{
        .identity = {.organization = "Retro++", .application = "Worker Pool"},
        .window   = {.title = "Polyrhythm — a worker pool beside the same room stepped inline"},
        .viewport = ViewportResolution{kViewW, kViewH}};
}

// The workers a pool made with no count starts here: none on one core, otherwise one fewer than the
// cores, never fewer than one. No machine runs on its own thread in this program.
[[nodiscard]] std::size_t expectedWorkers(std::size_t cores) {
    return cores <= 1 ? 0 : std::max<std::size_t>(1, cores - 1);
}

[[nodiscard]] long long microsecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
        .count();
}

[[nodiscard]] long long nanosecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
        .count();
}

// ── --verify ────────────────────────────────────────────────────────────────────────────────────────

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what.c_str());
        if (!ok) ++failures;
    };
    // Unbuffered, so a run that stops early still shows every line it reached.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("worker_pool --verify: a pool's room holds a plain loop's state at every worker count\n\n");

    EngineConfig::setActive(demoConfig());
    std::unique_ptr<SdlPlatform> platform;
    try {
        platform = detail::SdlPlatformTestAccess::inputOnly(demoConfig());
    } catch (const std::runtime_error& e) {
        std::printf("skipped: %s\n", e.what());
        return 77;
    }

    const std::size_t cores = platform->coreCount();
    {
        const WorkerPool pool{*platform};
        check(pool.workers() == expectedWorkers(cores),
              "WorkerPool{platform} started " + std::to_string(pool.workers()) + " workers on " +
                  std::to_string(cores) + " cores (expected " + std::to_string(expectedWorkers(cores)) + ")");
    }

    constexpr std::size_t   kActors = 256;
    constexpr std::uint32_t kTicks  = 600;

    WorkerPool machinePool{*platform};
    WorkerPool noWorkers{*platform, 0};
    WorkerPool threeWorkers{*platform, 3};

    struct Way {
        std::string       name;
        WorkerPool*       pool;
        Room              room;
        WorkerPool::Batch            batch;
        std::optional<std::uint32_t> firstMismatch;  // the first tick its room differed
    };
    std::array<Way, 3> ways{
        Way{.name = "WorkerPool{platform} (" + std::to_string(machinePool.workers()) + " workers)",
            .pool = &machinePool},
        Way{.name = "WorkerPool{platform, 0}", .pool = &noWorkers},
        Way{.name = "WorkerPool{platform, 3}", .pool = &threeWorkers},
    };

    Room reference;
    reference.reset(kActors);
    for (Way& way : ways) way.room.reset(kActors);

    // A tick: last tick's steps collected and applied, the rooms compared, then this tick's snapshot
    // gathered and its steps run — by the plain loop at once, by each pool as a batch handed over.
    const auto settle = [&](std::uint32_t tick) {
        reference.apply();
        for (Way& way : ways) {
            way.batch.collect();
            way.room.apply();
            if (!way.firstMismatch && !way.room.sameAs(reference)) way.firstMismatch = tick;
        }
    };
    for (std::uint32_t tick = 0; tick < kTicks; ++tick) {
        settle(tick);

        const Cell beacon = beaconAt(tick);
        reference.gather(beacon, tick);
        for (std::size_t i = 0; i < kActors; ++i) reference.step(i, kStartWork);
        for (Way& way : ways) {
            way.room.gather(beacon, tick);
            way.batch = submitSteps(*way.pool, way.room, kStartWork);
        }
    }
    settle(kTicks);
    for (const Way& way : ways) {
        check(!way.firstMismatch,
              way.name + ": the plain loop's state after every one of " + std::to_string(kTicks) + " ticks" +
                  (way.firstMismatch ? " — first differs at tick " + std::to_string(*way.firstMismatch) : ""));
    }

    // What submit costs the calling thread, and what collecting a finished batch costs. Printed, never
    // asserted: the figures are this machine's.
    std::printf("\n  timings on this machine (printed, not checked):\n");
    std::vector<std::size_t> sink(100'000);
    std::size_t* const       out = sink.data();
    for (const std::size_t count : {std::size_t{1}, std::size_t{1'000}, std::size_t{100'000}}) {
        const auto        start = std::chrono::steady_clock::now();
        WorkerPool::Batch batch = machinePool.submit(count, [out](std::size_t i) { out[i] = i; });
        const long long   ns    = nanosecondsSince(start);
        batch.collect();
        std::printf("    submit of %6zu jobs  %8lld ns\n", count, ns);
    }
    {
        WorkerPool::Batch batch = machinePool.submit(1'000, [out](std::size_t i) { out[i] = i; });
        while (!batch.done()) std::this_thread::yield();
        const auto start = std::chrono::steady_clock::now();
        batch.collect();
        std::printf("    collect of a finished batch  %lld ns\n", nanosecondsSince(start));
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}

// ── The window ──────────────────────────────────────────────────────────────────────────────────────

// The font sheet carries digits, then letters, then a blank. Anything else lands on the blank.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    return 36;
}

// `n` right-aligned in `width` columns, so a rolling digit never shifts its row.
[[nodiscard]] std::string pad(long long n, int width) {
    std::string s = std::to_string(n);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), ' ');
    return s;
}

int runWindow() {
    const EngineConfig config = demoConfig();
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};
    WorkerPool  pool{platform};
    WorkerPool  inlinePool{platform, 0};

    ActionMap map{
        {Action::FewerActors, {SDL_SCANCODE_LEFTBRACKET, PadButton::DpadLeft}},
        {Action::MoreActors, {SDL_SCANCODE_RIGHTBRACKET, PadButton::DpadRight}},
        {Action::LessWork, {SDL_SCANCODE_MINUS, PadButton::DpadDown}},
        {Action::MoreWork, {SDL_SCANCODE_EQUALS, PadButton::DpadUp}},
    };
    platform.actions(map);

    // ── The font and its three palettes ──────────────────────────────────────────────────────────
    const AtlasManifest font =
        renderer.loadAtlas("examples/worker_pool/assets/art/font.png", AssetDimensions{kGlyphPx, kGlyphPx},
                           ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                           TransparentIndices::of({0}), 0, AssetPolicy::Embed);
    const PaletteId palText = renderer.loadPaletteImage("examples/worker_pool/assets/palettes/font.png",
                                                        ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palLive = renderer.loadPaletteImage("examples/worker_pool/assets/palettes/font_pick.png",
                                                        ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palDim  = renderer.loadPaletteImage("examples/worker_pool/assets/palettes/mono.png",
                                                        ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const std::array<PaletteId, 3> actorPalettes{palText, palLive, palDim};

    // A glyph is 16px and the grid is 8px tiles, so each character stamps a 2×2 block.
    constexpr int         kMonW = kCols * 2, kMonH = kRows * 2;
    std::vector<TileCell> mon(static_cast<std::size_t>(kMonW) * kMonH,
                              TileCell{.atlas = font.atlasId, .tile = 0, .palette = palDim});
    const auto clearMon = [&] {
        for (TileCell& c : mon) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palDim;
        }
    };
    const auto write = [&](int col, int row, std::string_view text, PaletteId pal) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kCols || row < 0 || row >= kRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(text[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = mon[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile    = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette = pal;
                }
        }
    };

    // ── The two rooms ────────────────────────────────────────────────────────────────────────────
    std::size_t actorCount = kStartActors;
    int         work       = kStartWork;
    Room        pooled, inlined;
    pooled.reset(actorCount);
    inlined.reset(actorCount);
    WorkerPool::Batch pooledBatch, inlinedBatch;

    std::uint32_t                tick = 0;
    Cell                         beacon = beaconAt(0);
    long long                    pooledCollectUs = 0, inlinedCollectUs = 0;
    std::array<bool, kShareTicks> finishedEarly{};
    std::size_t                  shareAt = 0;
    bool                         roomsMatch = true;

    loop.simTick([&](const InputState& input) {
        // Last tick's steps: the pool's normally finished already, the inline pool's all run here.
        finishedEarly[shareAt] = pooledBatch.done();
        shareAt                = (shareAt + 1) % kShareTicks;
        auto start             = std::chrono::steady_clock::now();
        pooledBatch.collect();
        pooledCollectUs = microsecondsSince(start);
        start           = std::chrono::steady_clock::now();
        inlinedBatch.collect();
        inlinedCollectUs = microsecondsSince(start);

        pooled.apply();
        inlined.apply();
        roomsMatch = pooled.sameAs(inlined);

        const std::size_t countBefore = actorCount;
        if (input.justPressed(Action::FewerActors)) actorCount = std::max(kMinActors, actorCount / 2);
        if (input.justPressed(Action::MoreActors)) actorCount = std::min(kMaxActors, actorCount * 2);
        if (input.justPressed(Action::LessWork)) work = std::max(kMinWork, work / 2);
        if (input.justPressed(Action::MoreWork)) work = std::min(kMaxWork, work * 2);
        if (actorCount != countBefore) {
            pooled.reset(actorCount);
            inlined.reset(actorCount);
        }

        // The snapshot each step reads, then the steps handed over for the workers to run while this
        // thread renders and paces.
        beacon = beaconAt(tick);
        pooled.gather(beacon, tick);
        inlined.gather(beacon, tick);
        pooledBatch  = submitSteps(pool, pooled, work);
        inlinedBatch = submitSteps(inlinePool, inlined, work);
        ++tick;
    });

    FrameDrawState frame;
    const auto     drawRoom = [&](const Room& room, int col) {
        for (const Actor& actor : room.actors) {
            write(col + actor.at.x, kRoomRow + actor.at.y, std::string(1, static_cast<char>('0' + actor.digit)),
                  actorPalettes[actor.color]);
        }
        write(col + beacon.x, kRoomRow + beacon.y, "X", palLive);
    };
    const auto buildFrame = [&]() {
        clearMon();
        write(kLeftCol, 0, "POOL", palText);
        write(kRightCol, 0, "INLINE", palText);
        drawRoom(pooled, kLeftCol);
        drawRoom(inlined, kRightCol);

        const auto finished = static_cast<long long>(std::count(finishedEarly.begin(), finishedEarly.end(), true));
        write(1, 17, "WORKERS", palDim);
        write(17, 17, pad(static_cast<long long>(pool.workers()), 6), palLive);
        write(24, 17, "OF " + std::to_string(platform.coreCount()) + " CORES", palDim);
        write(1, 18, "ACTORS", palDim);
        write(17, 18, pad(static_cast<long long>(actorCount), 6), palLive);
        write(24, 18, "LEFT AND RIGHT BRACKET", palText);
        write(1, 19, "WORK", palDim);
        write(17, 19, pad(work, 6), palLive);
        write(24, 19, "MINUS AND EQUALS", palText);
        write(1, 20, "POOL COLLECT", palDim);
        write(17, 20, pad(pooledCollectUs, 6), palText);
        write(24, 20, "US  DONE " + std::to_string(finished) + " OF " + std::to_string(kShareTicks), palDim);
        write(1, 21, "INLINE COLLECT", palDim);
        write(17, 21, pad(inlinedCollectUs, 6), palText);
        write(24, 21, "US", palDim);
        write(1, 22, "ROOMS MATCH", palDim);
        write(17, 22, roomsMatch ? "   YES" : "    NO", roomsMatch ? palText : palLive);

        frame.layers.clear();
        DrawLayer panel{.key = "panel"};
        panel.z       = 0;
        panel.size    = PixelSize{kViewW, kViewH};
        panel.content = TileContent{.widthInTiles  = kMonW,
                                    .heightInTiles = kMonH,
                                    .cells         = std::span<const TileCell>(mon),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(panel);
    };

    loop.renderLoop([&]() {
        buildFrame();
        renderer.renderFrame(frame);
    });

    std::printf(
        "worker pool — the left room's steps run on the pool's workers while the frame renders; the right\n"
        "room's run when its batch is collected. LEFT and RIGHT BRACKET halve and double the actors, MINUS\n"
        "and EQUALS halve and double the work each step does. ROOMS MATCH compares the two every tick.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) return runVerify();
    return runWindow();
}
