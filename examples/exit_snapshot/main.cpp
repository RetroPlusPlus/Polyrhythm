// Exit snapshot — the application-exit guard as a visible round-trip, on top of the persistence core.
//
// Two ways to quit, each running a close-out guard that saves a snapshot before the program tears down:
//   • press C — a CALLBACK guard: it writes this session's numbers to the "snapshot_callback" document
//     and returns Proceed immediately (the one-shot close-out).
//   • press G — a MULTI-TICK guard: it plays a short fade (returning NotYet each frame while the sim
//     keeps advancing), then writes this session's numbers to the "snapshot_gates" document and
//     Proceeds (the NotYet→Proceed close-out — the guard's reason to exist).
//   • Esc or the window's close button — a PLAIN quit: the guard saves nothing and Proceeds. Even the
//     OS close routes through the SAME guard.
//
// This session picks a fresh random set of numbers at startup and shows them as white bars. On relaunch
// it loads whichever snapshots exist and shows them below (green = callback, cyan = gates). The random
// is load-bearing: the loaded bars match the PREVIOUS session's fresh bars, not this session's — proof
// the numbers were RESTORED from disk, not regenerated. Two documents keep the two exit paths
// independently verifiable in one program.
//
// Built on every CI platform (so it keeps compiling against the live engine); run it on a dev machine —
// CI has no display. Run it twice: quit with C once, relaunch and the green bars are there; quit with G,
// relaunch and the cyan bars join them.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/save_store.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

// The demo's input vocabulary — the two snapshot quits and a plain quit. A game names its own actions.
enum class Action : std::uint8_t { SaveViaCallback, SaveViaGates, QuitPlain };

// Which close-out the guard runs when an exit is pending. Set just before exitRequest(); the single
// registered guard switches on it. Plain is also what the OS window-close resolves to (no snapshot).
enum class ExitMode : std::uint8_t { Plain, Callback, Gates };

constexpr std::size_t kNumbers = 4;                 // how many values a snapshot holds
using Snapshot = std::array<std::uint8_t, kNumbers>;

// A snapshot is just its bytes on disk — the store never interprets them.
std::vector<std::byte> toBytes(const Snapshot& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}
std::optional<Snapshot> readSnapshot(const SaveStore& store, std::string_view name) {
    const auto doc = store.read(name);
    if (!doc || doc->payload.size() != kNumbers) return std::nullopt;
    Snapshot s{};
    for (std::size_t i = 0; i < kNumbers; ++i) s[i] = static_cast<std::uint8_t>(doc->payload[i]);
    return s;
}

void printSnapshot(const char* label, const std::optional<Snapshot>& s) {
    std::printf("%-22s", label);
    if (!s) { std::printf("(none yet)\n"); return; }
    for (const std::uint8_t v : *s) std::printf(" %3u", v);
    std::printf("\n");
}

// A color-fill bar whose length encodes a number (0–255 → 0–maxLen viewport px).
Region bar(std::string key, float x, float y, std::uint8_t value, float maxLen, Rgba8 colour) {
    const float len = 2.0f + (static_cast<float>(value) / 255.0f) * maxLen;  // min stub so a 0 still reads
    return Region{
        .key     = std::move(key),
        .shape   = ShapePoints::rectangle(Point{x, y}, len, 6.0f),
        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour}}};
}

// One column of bars for a snapshot (or nothing when it is absent). Appends into `out`.
void appendColumn(std::vector<Region>& out, const char* tag, float x, float y,
                  const std::optional<Snapshot>& snap, Rgba8 colour) {
    if (!snap) return;
    for (std::size_t i = 0; i < kNumbers; ++i) {
        out.push_back(bar(std::string(tag) + std::to_string(i), x,
                          y + static_cast<float>(i) * 10.0f, (*snap)[i], 40.0f, colour));
    }
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "ExitSnapshotDemo"},
        .window   = {.title = "Polyrhythm — exit snapshot"}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};
    RunLoop     loop{clock};

    // A default-constructed store resolves the per-user save directory from the active config's identity.
    SaveStore store;
    std::printf("save directory: %s\n\n", store.basePath().string().c_str());

    // Load whatever previous sessions left behind (either, both, or neither exist).
    const std::optional<Snapshot> loadedCallback = readSnapshot(store, "snapshot_callback");
    const std::optional<Snapshot> loadedGates    = readSnapshot(store, "snapshot_gates");

    // This session's fresh numbers — seeded off the wall clock, so they differ every launch. After a
    // relaunch the LOADED numbers match a PRIOR session's fresh numbers, never this run's → the restore
    // is visible, not a coincidence.
    std::mt19937 rng(static_cast<std::uint32_t>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    Snapshot fresh{};
    for (auto& v : fresh) v = static_cast<std::uint8_t>(rng() % 256);

    std::printf("this session (white):  ");
    for (const std::uint8_t v : fresh) std::printf(" %3u", v);
    std::printf("\n");
    printSnapshot("loaded callback (green):", loadedCallback);
    printSnapshot("loaded gates    (cyan): ", loadedGates);
    std::printf("\nC = save via callback + quit   G = save via gates + quit   Esc / window-close = plain quit\n");

    ActionMap map;
    map.bind(Action::SaveViaCallback, SDL_SCANCODE_C);
    map.bind(Action::SaveViaGates,    SDL_SCANCODE_G);
    map.bind(Action::QuitPlain,       SDL_SCANCODE_ESCAPE);
    platform.actions(map);

    // The single close-out guard. It reads `mode` (set by the tick just before exitRequest) and the OS
    // close leaves it Plain. Callback saves and Proceeds at once; Gates plays a fade (NotYet) then saves
    // and Proceeds; Plain saves nothing.
    ExitMode mode      = ExitMode::Plain;
    int      fadeLeft  = 0;               // remaining fade frames for the Gates close-out
    loop.exitAction([&]() -> ExitVerdict {
        switch (mode) {
            case ExitMode::Callback:
                store.write("snapshot_callback", 1, toBytes(fresh));
                std::printf("callback guard: wrote snapshot_callback — proceeding.\n");
                return ExitVerdict::Proceed;
            case ExitMode::Gates:
                if (fadeLeft > 0) { --fadeLeft; return ExitVerdict::NotYet; }  // still fading
                store.write("snapshot_gates", 1, toBytes(fresh));
                std::printf("gates guard: fade done, wrote snapshot_gates — proceeding.\n");
                return ExitVerdict::Proceed;
            case ExitMode::Plain:
            default:
                std::printf("plain quit: no snapshot.\n");
                return ExitVerdict::Proceed;
        }
    });

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::SaveViaCallback)) { mode = ExitMode::Callback; loop.exitRequest(); }
        if (in.justPressed(Action::SaveViaGates))    { mode = ExitMode::Gates; fadeLeft = 30; loop.exitRequest(); }
        if (in.justPressed(Action::QuitPlain))       { mode = ExitMode::Plain; loop.exitRequest(); }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "bars"};
        layer.z    = 0;
        layer.size = PixelSize{config.viewport.width, config.viewport.height};

        std::vector<Region> regions;
        // A dim backdrop so the bars read; then a labeled column per snapshot.
        regions.push_back(Region{
            .key     = "backdrop",
            .shape   = ShapePoints::rectangle(Point{0.0f, 0.0f},
                                              static_cast<float>(config.viewport.width),
                                              static_cast<float>(config.viewport.height)),
            .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{24, 24, 34}}}});

        // This session's fresh numbers (white), always shown.
        for (std::size_t i = 0; i < kNumbers; ++i)
            regions.push_back(bar("fresh" + std::to_string(i), 8.0f, 12.0f + static_cast<float>(i) * 10.0f,
                                  fresh[i], 40.0f, Rgba8{235, 235, 245}));
        appendColumn(regions, "cb", 60.0f, 12.0f, loadedCallback, Rgba8{90, 200, 120});
        appendColumn(regions, "gt", 112.0f, 12.0f, loadedGates, Rgba8{90, 200, 220});

        layer.regions = std::move(regions);
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    WindowedHost{loop, platform}.run();
    std::printf("\nrun me again — the snapshot bars you saved will be there.\n");
    return 0;
}
