// Sprite-patrol demo — a runnable host that VISUALLY proves the SpritePath SEQUENCE + INTERRUPT layer: it
// opens a window and drives five movers in the 160×144 viewport, each exercising one part of the sequencing /
// interrupt-stack orchestrator, so every shipped capability is visible at once:
//
//   1. GUARD     — a walk-cycle patrol LOOPING a route of chained legs, no authored origins (each leg departs
//                  from the previous leg's end): a straight march (Move::to), a corner WAIT node (zero-length
//                  Move + PathPacing::eased), a multi-point sweep (Move::through), and a Hermite arc back;
//                  FlipX faces travel; per-leg pacing variety. Sequence mode LoopIndefinitely.
//   2. COURIER   — runs its route ONCE and rests at the end, walk cycle still playing at rest: a first leg,
//                  a mid-route EXPLICIT-ORIGIN leg (a jump), and an Move::onCurve leg. Sequence mode single().
//   3. INSPECTOR — laps a small yard loopNTimes(2) then holds.
//   4. SIBLING   — an arrow drifting under playForDuration(10 s): holds at the cutoff.
//   5. SENTRY    — a SENTINEL node (default Speed 0 on nonzero geometry): stands post, breathing (a scale
//                  track), until an interrupt or a re-path moves it. Never finishes.
//
// The interrupt stack (every control is an action bound to a key AND a pad button — main()'s ActionMap):
//   S (pad west)       — a DETOUR on the GUARD (departs from where he stands), ResumePolicy::Continue: on
//                        finish the patrol carries on from where the detour ended — the whole route drifts
//                        down, it does not snap back.
//   A (pad north)      — a DODGE on top (depth 2), ResumePolicy::Return: on finish it snaps the guard back
//                        to where he was.
//   Q (left shoulder)  — a LoopIndefinitely CHASE interrupt on the SENTRY (its own captured mode: never
//                        auto-pops).
//   W (right shoulder) — POP the SENTRY's chase (the only exit from a LoopIndefinitely interrupt).
//
// The GUARD's player surface: X (pad south) play/pause, Z (pad east) restart, Return (pad Start) stop (both
// clear its interrupts), ←/→ or the d-pad seek ±1 s (lands correctly across leg boundaries). Backspace (pad
// Select) fullscreen. Close to quit. Node-label transitions and interrupt depth are logged to stdout as they
// happen.
//
// This is the visual sanity check for a pure-CPU layer — the device-free ctest suite is the real gate. The
// window never auto-launches — a dev drives it.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "retropp/animation.h"
#include "retropp/clock.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"  // ActionMap, PadButton — the demo's control scheme
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/sprite_path.h"
#include "retropp/tween.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

// The demo's input vocabulary; the ActionMap in main() binds each action to its key and pad
// button, so keyboard and controller both drive the movers.
enum class Action : std::uint8_t {
    PlayPause,    // guard: toggle play/pause
    Restart,      // guard: restart (clears interrupts)
    Stop,         // guard: stop (clears interrupts)
    SeekBack,     // guard: seek -1 s
    SeekForward,  // guard: seek +1 s
    Detour,       // guard: push the Continue detour
    Dodge,        // guard: push the Return dodge on top (depth 2)
    Chase,        // sentry: push the looping chase
    PopChase,     // sentry: pop the chase (its only exit)
    Fullscreen,   // toggle fullscreen
};

// An 8×8 right-pointing arrow (index 1 = body, index 0 = the OBJ hole); at rotation 0 it points +x, so a
// RotateToFacing transform aims it along travel.
constexpr const char* kArrow[8] = {
    "........", "...##...", "...###..", "#######.", "########", "#######.", "...###..", "...##...",
};

// Two walk frames (legs apart / legs together) for the walk-cycle animation.
constexpr const char* kWalkA[8] = {
    "..####..", "..####..", ".######.", ".######.", "..####..", "..####..", ".#....#.", "##....##",
};
constexpr const char* kWalkB[8] = {
    "..####..", "..####..", ".######.", ".######.", "..####..", "..####..", "..#..#..", "..#..#..",
};

// Fill an 8×8 index cell of an atlas byte buffer at cell column `cx` (rows are `stride` wide).
void blit8(std::uint8_t* buf, int stride, int cx, const char* const art[8]) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            buf[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(cx * 8 + x)] =
                art[y][x] == '#' ? 1 : 0;
}

// A mover paired with the sequence mode it plays under and a bit of transition-logging state.
struct Mover {
    SpritePath           path;
    PlaybackMode         mode;
    const char*          name;
    std::string_view     lastLabel;
    std::size_t          lastDepth = 0;

    void advance() { path.advance(mode); }

    // Log node-label transitions and interrupt depth changes to stdout (row 8 of the coverage table).
    void logTransitions() {
        const SpritePathNode* cur   = path.currentNode();
        const std::string_view label = cur ? cur->label : std::string_view{};
        if (label != lastLabel) {
            std::printf("[%s] node → %.*s\n", name, static_cast<int>(label.size()), label.data());
            lastLabel = label;
        }
        if (path.interruptDepth() != lastDepth) {
            std::printf("[%s] interrupt depth → %zu\n", name, path.interruptDepth());
            lastDepth = path.interruptDepth();
        }
    }
};

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Patrol Demo"},
        .window = {.title = "Polyrhythm — sprite patrol (sequence + interrupts)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The control scheme: one key + one pad button per action. The interrupt controls sit on
    // S/A/Q/W (and the pad's four remaining face/shoulder buttons); the guard's player surface
    // rides X/Z/Return and the arrows; seek directions come from the arrows + d-pad only, so the
    // S/A/Q/W letter keys stay free for the interrupts.
    const ActionMap map{
        {Action::PlayPause,   {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Restart,     {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Stop,        {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::SeekBack,    {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::SeekForward, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Detour,      {SDL_SCANCODE_S, PadButton::FaceWest}},
        {Action::Dodge,       {SDL_SCANCODE_A, PadButton::FaceNorth}},
        {Action::Chase,       {SDL_SCANCODE_Q, PadButton::ShoulderL}},
        {Action::PopChase,    {SDL_SCANCODE_W, PadButton::ShoulderR}},
        {Action::Fullscreen,  {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // ── Atlases ───────────────────────────────────────────────────────────────────────────────────────
    std::array<std::uint8_t, 64> arrowArt{};
    blit8(arrowArt.data(), 8, 0, kArrow);
    const AtlasId arrowAtlas = renderer.uploadAtlas(arrowArt.data(), 8, 8, TransparentIndices::GameBoy).atlasId;

    std::array<std::uint8_t, 64> blockArt{};  // fully solid (index 1)
    blockArt.fill(1);
    const AtlasId blockAtlas = renderer.uploadAtlas(blockArt.data(), 8, 8).atlasId;

    std::array<std::uint8_t, 128> walkArt{};  // 16×8 — two 8×8 frames side by side (tile 0, tile 1)
    blit8(walkArt.data(), 16, 0, kWalkA);
    blit8(walkArt.data(), 16, 1, kWalkB);
    // The upload declares the carve: two 8×8 cells → the walk sheet (slot 0 = cell 0, slot 1 = cell 1).
    const AtlasManifest walkSheet =
        renderer.uploadAtlas(walkArt.data(), 16, 8, AssetDimensions::GameBoy8x8,
                             ContentKind::SpriteSeries, ReadOrder::LeftRightThenDown, 0,
                             TransparentIndices::GameBoy);

    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;

    // ── Palettes (entry [1] is the visible color; [0] the hole for the arrow / walk sheets) ────────────
    const std::array<Rgba8, 2> guardPal{{{0, 0, 0}, {120, 220, 140}}};      // green
    const std::array<Rgba8, 2> courierPal{{{0, 0, 0}, {120, 200, 255}}};    // cyan
    const std::array<Rgba8, 2> inspectorPal{{{0, 0, 0}, {200, 140, 255}}};  // violet
    const std::array<Rgba8, 2> siblingPal{{{0, 0, 0}, {255, 180, 90}}};     // orange
    const std::array<Rgba8, 2> sentryPal{{{0, 0, 0}, {255, 120, 160}}};     // pink
    const PaletteId guardPalId     = renderer.uploadPalette(std::span<const Rgba8>(guardPal));
    const PaletteId courierPalId   = renderer.uploadPalette(std::span<const Rgba8>(courierPal));
    const PaletteId inspectorPalId = renderer.uploadPalette(std::span<const Rgba8>(inspectorPal));
    const PaletteId siblingPalId   = renderer.uploadPalette(std::span<const Rgba8>(siblingPal));
    const PaletteId sentryPalId    = renderer.uploadPalette(std::span<const Rgba8>(sentryPal));

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {18, 22, 34}, {34, 44, 66}}};
    const PaletteId             gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell> gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // ── Game-owned data the movers reference (must outlive them) ────────────────────────────────────────
    // The walk cycle's frames carry a nominal palette; each walk mover overrides it after applyTo, so one
    // shared Animation dresses in five colors. Each frame names the walk sheet + a slot index.
    const Animation walkCycle{
        .frames = {AnimationFrame{.sheet = walkSheet.atlasId, .tileIndex = 0, .palette = guardPalId, .duration = 200ms},
                   AnimationFrame{.sheet = walkSheet.atlasId, .tileIndex = 1, .palette = guardPalId, .duration = 200ms}}};

    const Curve courierArc = Curve::quadratic({55, 50}, {100, 12}, {145, 50});  // COURIER's onCurve leg

    // ── The five movers ─────────────────────────────────────────────────────────────────────────────────

    // 1. GUARD — a looping route of chained legs (no authored origins). Covers origin chaining, per-node
    //    variety (move kinds + pacings + tracks), the WAIT idiom, sequence LoopIndefinitely, node-local
    //    animation restart per pass.
    Mover guard{
        .path = SpritePath{.nodes = {{.label     = "march",
                                      .move      = SpritePathMove::to({130, 112}),
                                      .pacing    = PathPacing::speed(30.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label     = "halt",  // WAIT node: zero-length move + eased duration
                                      .move      = SpritePathMove::to({130, 112}),
                                      .pacing    = PathPacing::eased(1500ms),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label     = "sweep",
                                      .move      = SpritePathMove::through({{130, 84}, {40, 84}}),
                                      .pacing    = PathPacing::speed(30.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label     = "return",
                                      .move      = SpritePathMove::hermite({30, 112}, {-40, 60}, {40, 30}),
                                      .pacing    = PathPacing::speed(30.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle}},
                           .start = {30, 112}},
        .mode = PlaybackMode::loopIndefinitely(),
        .name = "guard"};

    // 2. COURIER — one pass then rest, walk cycle still playing at rest; a mid-route explicit-origin jump and
    //    an onCurve leg.
    Mover courier{
        .path = SpritePath{.nodes = {{.label     = "leg1",
                                      .move      = SpritePathMove::to({70, 25}),
                                      .pacing    = PathPacing::speed(34.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label     = "hop",  // EXPLICIT origin — a jump back to (15,25)
                                      .move      = SpritePathMove::to({15, 25}, {55, 50}),
                                      .pacing    = PathPacing::speed(34.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label     = "arc",  // onCurve — travels the pre-authored quadratic
                                      .move      = SpritePathMove::onCurve(courierArc),
                                      .pacing    = PathPacing::speed(34.0f),
                                      .facing    = FacingPolicy::FlipX,
                                      .animation = &walkCycle}},
                           .start = {15, 25}},
        .mode = PlaybackMode::single(),
        .name = "courier"};

    // 3. INSPECTOR — laps a small yard twice, then holds.
    Mover inspector{
        .path = SpritePath{.nodes = {{.label = "east", .move = SpritePathMove::to({120, 58}),
                                      .pacing = PathPacing::speed(44.0f), .facing = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label = "south", .move = SpritePathMove::to({120, 74}),
                                      .pacing = PathPacing::speed(44.0f), .animation = &walkCycle},
                                     {.label = "west", .move = SpritePathMove::to({95, 74}),
                                      .pacing = PathPacing::speed(44.0f), .facing = FacingPolicy::FlipX,
                                      .animation = &walkCycle},
                                     {.label = "north", .move = SpritePathMove::to({95, 58}),
                                      .pacing = PathPacing::speed(44.0f), .animation = &walkCycle}},
                           .start = {95, 58}},
        .mode = PlaybackMode::loopNTimes(2),
        .name = "inspector"};

    // 4. SIBLING — an arrow drifting under playForDuration(10 s); holds at the cutoff.
    Mover sibling{
        .path = SpritePath{.nodes = {{.label  = "drift",
                                      .move   = SpritePathMove::to({150, 40}),
                                      .pacing = PathPacing::speed(16.0f),
                                      .facing = FacingPolicy::RotateToFacing}},
                           .start = {12, 40}},
        .mode = PlaybackMode::playForDuration(10s),
        .name = "sibling"};

    // 5. SENTRY — a sentinel node (nonzero geometry, default Speed 0 → never finishes): stands post at its
    //    origin, breathing, until the chase interrupt (Q) moves it.
    Mover sentry{
        .path = SpritePath{.nodes = {{.label = "post",
                                      .move  = SpritePathMove::to({120, 120}),  // nonzero geometry, Speed 0
                                      .scale = Tween<Vec2>::of({1.0f, 1.0f}, {1.4f, 1.4f}, 1400ms, Easing::InOutSine)
                                                   .then({1.0f, 1.0f}, 1400ms, Easing::InOutSine),
                                      .scaleMode = PlaybackMode::loopIndefinitely()}},
                           .start = {80, 120}},
        .mode = PlaybackMode::loopIndefinitely(),  // the sentinel rests regardless; mode is moot
        .name = "sentry"};

    std::vector<Mover*> all{&guard, &courier, &inspector, &sibling, &sentry};
    bool                     paused = false;
    std::chrono::nanoseconds seekAt{0};

    loop.simTick([&](const InputState& in) {
        // GUARD player surface.
        if (in.justPressed(Action::PlayPause)) {
            paused = !paused;
            paused ? guard.path.pause() : guard.path.play();
            std::printf("[dev] guard %s\n", paused ? "paused" : "playing");
        }
        if (in.justPressed(Action::Restart)) {
            guard.path.restart();
            seekAt = 0ns;
            paused = false;
            std::printf("[dev] guard restarted (interrupts cleared)\n");
        }
        if (in.justPressed(Action::Stop)) {
            guard.path.stop();
            std::printf("[dev] guard stopped (interrupts cleared)\n");
        }
        if (in.justPressed(Action::SeekForward)) {
            seekAt += 1s;
            guard.path.seek(seekAt);
            std::printf("[dev] guard seek +1s (%lld ms)\n", static_cast<long long>(seekAt / 1ms));
        }
        if (in.justPressed(Action::SeekBack)) {
            seekAt = seekAt > 1s ? seekAt - 1s : 0ns;
            guard.path.seek(seekAt);
            std::printf("[dev] guard seek -1s (%lld ms)\n", static_cast<long long>(seekAt / 1ms));
        }

        // The interrupt stack.
        if (in.justPressed(Action::Detour)) {  // GUARD detour, Continue: the patrol drifts on from where it ends
            guard.path.interrupt({{.label     = "detour",
                                   .move      = SpritePathMove::to({80, 44}),
                                   .pacing    = PathPacing::speed(52.0f),
                                   .facing    = FacingPolicy::FlipX,
                                   .animation = &walkCycle}},
                                 PlaybackMode::single(), ResumePolicy::Continue);
            std::printf("[dev] guard DETOUR — Continue (depth %zu)\n", guard.path.interruptDepth());
        }
        if (in.justPressed(Action::Dodge)) {  // a DODGE on top (depth 2), Return: snaps back on finish
            guard.path.interrupt({{.label     = "dodge",
                                   .move      = SpritePathMove::to({40, 30}),
                                   .pacing    = PathPacing::speed(64.0f),
                                   .facing    = FacingPolicy::FlipX,
                                   .animation = &walkCycle}},
                                 PlaybackMode::single(), ResumePolicy::Return);
            std::printf("[dev] guard DODGE — Return (depth %zu)\n", guard.path.interruptDepth());
        }
        if (in.justPressed(Action::Chase)) {  // SENTRY chase (its own LoopIndefinitely mode)
            if (!sentry.path.interrupted()) {
                sentry.path.interrupt({{.label  = "chase",
                                        .move   = SpritePathMove::through({{120, 96}, {90, 96}, {90, 120}}),
                                        .pacing = PathPacing::speed(46.0f),
                                        .facing = FacingPolicy::FlipX}},
                                      PlaybackMode::loopIndefinitely());
                std::printf("[dev] sentry CHASE (loop interrupt; W to pop)\n");
            }
        }
        if (in.justPressed(Action::PopChase)) {  // POP the sentry chase (its only exit)
            if (sentry.path.interrupted()) {
                sentry.path.popInterrupt();
                std::printf("[dev] sentry chase POPPED — back on post\n");
            }
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());

        for (Mover* m : all) {
            if (!(m == &guard && paused)) m->advance();
            m->logTransitions();
        }
    });

    std::vector<Sprite> movers;
    FrameDrawState      frame;
    loop.renderLoop([&]() {
        movers.clear();

        // Walk-cycle movers: applyTo writes the frame art + position (+ flip); override the palette so one
        // shared Animation dresses in its own color.
        const auto walker = [&](Mover& m, PaletteId pal, const char* key) {
            Sprite s{.key = key};
            m.path.applyTo(s);
            s.palette = pal;
            movers.push_back(s);
        };
        // Static-art movers: set art first (no animation track), then applyTo writes position (+ transform).
        const auto arrow = [&](Mover& m, PaletteId pal, const char* key) {
            Sprite s{.key = key};
            s.atlas   = arrowAtlas;
            s.palette = pal;
            s.tile    = 0;
            s.size    = AssetDimensions::GameBoy8x8;
            m.path.applyTo(s);
            movers.push_back(s);
        };
        const auto block = [&](Mover& m, PaletteId pal, const char* key) {
            Sprite s{.key = key};
            s.atlas   = blockAtlas;
            s.palette = pal;
            s.tile    = 0;
            s.size    = AssetDimensions::GameBoy8x8;
            m.path.applyTo(s);
            movers.push_back(s);
        };

        walker(guard, guardPalId, "guard");
        walker(courier, courierPalId, "courier");
        walker(inspector, inspectorPalId, "inspector");
        arrow(sibling, siblingPalId, "sibling");
        block(sentry, sentryPalId, "sentry");

        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles  = kMapW,
                                 .heightInTiles = kMapH,
                                 .cells         = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        DrawLayer moverLayer{.key = "movers"};
        moverLayer.z       = 10;
        moverLayer.size    = PixelSize{kViewW, kViewH};
        moverLayer.content = SpriteContent{.sprites = std::span<const Sprite>(movers)};
        frame.layers.push_back(std::move(moverLayer));

        renderer.renderFrame(frame);
    });

    std::printf("sprite patrol demo — five movers: guard (looping chained legs + wait node), courier (single "
                "pass, explicit-origin jump + onCurve, rests playing), inspector (loopNTimes 2), sibling "
                "(playForDuration 10s), sentry (sentinel post, breathing).\n"
                "  S detour guard · A dodge (depth 2) · Q sentry chase (loop) · W pop chase\n"
                "  X play/pause · Z restart · Return stop · ←/→ seek ±1s · Backspace fullscreen\n"
                "  (every control also rides a pad button: face buttons, shoulders, d-pad, Start/Select).\n"
                "  Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
