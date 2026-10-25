// Sprite-path demo — a runnable host that VISUALLY proves SpritePath: it opens a window and drives seven
// slow movers in the 160×144 viewport, each exercising one part of the movement orchestrator, so every
// shipped capability is visible at once:
//
//   1. WALKER  — a two-frame walk cycle pacing a straight patrol line, mirrored to face travel:
//                Move::through (a there-and-back line), PathPacing::speed, an Animation track looping, and
//                FacingPolicy::FlipX (which holds across the turnaround). Driven through applyTo().
//   2. FISH    — an arrow nosing around a multi-point loop: Move::through, PathPacing::eased, and
//                FacingPolicy::RotateToFacing (the sprite rotates to point along travel).
//   3. SHUTTLE — an arrow on a Hermite arc whose distance runs to the end, BACKS UP, then continues:
//                Move::hermite + PathPacing::distanceTween with a non-monotone (reversing) profile.
//   4. TUMBLER — a block spinning while it "breathes": a rotation track and a scale track running at once
//                about the sprite's default center pivot, FacingPolicy::None.
//   5. SWINGER — a block rotating about its top-left CORNER: a rotation track with a pivot override.
//   6. GLIDER  — an arrow riding a pre-authored cubic Curve: Move::onCurve, nosing along via RotateToFacing.
//   7. GHOST   — a translucent twin of the walker drawn from its RAW sample() (position + frame read, custom
//                offset write), beside applyTo().
//
// Controls (the full player surface): A play/pause all, B restart all, Start stop all, ←/→ seek all by ±1 s,
// Select fullscreen. Close to quit.
//
// This is the visual sanity check for a pure-CPU layer — the device-free ctest suite is the real gate. The
// window never auto-launches — a dev drives it.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/animation.h"
#include "retropp/clock.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
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

// The demo's input vocabulary — the full player surface: play/pause, restart, stop, seek both ways,
// and fullscreen.
enum class Action : std::uint8_t { PlayPause, Restart, Stop, SeekForward, SeekBack, Fullscreen };

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

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Path Demo"},
        .window = {.title = "Polyrhythm — sprite path (seven movers)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::PlayPause,   {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Restart,     {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Stop,        {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::SeekForward, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {Action::SeekBack,    {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::Fullscreen,  {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // ── Atlases ───────────────────────────────────────────────────────────────────────────────────────
    // The arrow and walker sheets use index 0 as their background, so upload them with index 0 as a
    // structural hole (TransparentIndices::GameBoy) — otherwise the padding draws as opaque black.
    std::array<std::uint8_t, 64> arrowArt{};
    blit8(arrowArt.data(), 8, 0, kArrow);
    const AtlasId arrowAtlas = renderer.uploadAtlas(arrowArt.data(), 8, 8, TransparentIndices::GameBoy).atlasId;

    std::array<std::uint8_t, 64> blockArt{};  // fully solid (index 1) — no hole needed
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

    // ── Palettes (entry [1] is the visible color; [0] unused / the hole) ───────────────────────────────
    const std::array<Rgba8, 2> walkerPal{{{0, 0, 0}, {120, 220, 140}}};   // green
    const std::array<Rgba8, 2> fishPal{{{0, 0, 0}, {120, 200, 255}}};     // cyan
    const std::array<Rgba8, 2> shuttlePal{{{0, 0, 0}, {255, 180, 90}}};   // orange
    const std::array<Rgba8, 2> tumblerPal{{{0, 0, 0}, {200, 140, 255}}};  // violet
    const std::array<Rgba8, 2> swingerPal{{{0, 0, 0}, {255, 120, 160}}};  // pink
    const std::array<Rgba8, 2> gliderPal{{{0, 0, 0}, {160, 255, 180}}};   // mint
    const std::array<Rgba8, 2> shadowPal{{{0, 0, 0}, {120, 130, 155}}};   // slate — a ghost twin that reads on the dark grid
    const PaletteId walkerPalId  = renderer.uploadPalette(std::span<const Rgba8>(walkerPal));
    const PaletteId fishPalId     = renderer.uploadPalette(std::span<const Rgba8>(fishPal));
    const PaletteId shuttlePalId  = renderer.uploadPalette(std::span<const Rgba8>(shuttlePal));
    const PaletteId tumblerPalId  = renderer.uploadPalette(std::span<const Rgba8>(tumblerPal));
    const PaletteId swingerPalId  = renderer.uploadPalette(std::span<const Rgba8>(swingerPal));
    const PaletteId gliderPalId   = renderer.uploadPalette(std::span<const Rgba8>(gliderPal));
    const PaletteId shadowPalId   = renderer.uploadPalette(std::span<const Rgba8>(shadowPal));

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {18, 22, 34}, {34, 44, 66}}};
    const PaletteId             gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell> gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // ── Game-owned data the movers reference (must outlive them) ────────────────────────────────────────
    // Each frame names the walk sheet + a slot index.
    const Animation walkCycle{.frames = {AnimationFrame{.sheet = walkSheet.atlasId, .tileIndex = 0,
                                                        .palette = walkerPalId, .duration = 220ms},
                                         AnimationFrame{.sheet = walkSheet.atlasId, .tileIndex = 1,
                                                        .palette = walkerPalId, .duration = 220ms}}};

    const float shuttleLen = Curve::hermite({20, 90}, {50, -70}, {140, 90}, {50, 70}).length();
    const Tween<float> shuttleProfile = Tween<float>::of(0.0f, shuttleLen, 5s)
                                            .then(shuttleLen * 0.35f, 2s)   // back up along the arc
                                            .then(shuttleLen, 3s);          // then continue to the end

    const Curve gliderCurve = Curve::cubic({12, 58}, {40, 6}, {120, 122}, {150, 52});

    // ── The seven movers ────────────────────────────────────────────────────────────────────────────────
    // 1. Walk-cycle patrol, mirrored to face travel.
    SpritePath walker{.nodes = {{.move      = SpritePathMove::through({{140, 118}, {20, 118}}),
                                 .pacing    = PathPacing::speed(26.0f),
                                 .facing    = FacingPolicy::FlipX,
                                 .animation = &walkCycle}},
                      .start = {20, 118}};

    // 2. Nose along a rounded loop, eased.
    SpritePath fish{.nodes = {{.move   = SpritePathMove::through({{120, 20}, {120, 50}, {40, 50}, {40, 20}}),
                               .pacing = PathPacing::eased(9s, Easing::InOutSine),
                               .facing = FacingPolicy::RotateToFacing}},
                    .start = {40, 20}};

    // 3. Hermite arc, distance runs forward / back / forward.
    SpritePath shuttle{.nodes = {{.move   = SpritePathMove::hermite({20, 90}, {140, 90}, {50, -70}, {50, 70}),
                                  .pacing = PathPacing::distanceTween(shuttleProfile)}},
                       .start = {20, 90}};

    // 4. Spin + breathe in place, default center pivot.
    SpritePath tumbler{.nodes = {{.move            = SpritePathMove::to({80, 66}),  // Speed 0 → parked at start
                                  .rotationDegrees = Tween<float>::of(0.0f, 360.0f, 6s, Easing::Linear),
                                  .rotationMode    = PlaybackMode::loopIndefinitely(),
                                  .scale           = Tween<Vec2>::of({1.0f, 1.0f}, {1.5f, 1.5f}, 2s, Easing::InOutSine)
                                                         .then({1.0f, 1.0f}, 2s, Easing::InOutSine),
                                  .scaleMode       = PlaybackMode::loopIndefinitely()}},
                       .start = {80, 66}};

    // 5. Rotate about the top-left corner (pivot override).
    SpritePath swinger{.nodes = {{.move            = SpritePathMove::to({128, 108}),
                                  .rotationDegrees = Tween<float>::of(0.0f, 360.0f, 4s, Easing::Linear),
                                  .rotationMode    = PlaybackMode::loopIndefinitely(),
                                  .pivot           = Vec2{0.0f, 0.0f}}},
                       .start = {128, 108}};

    // 6. Ride a pre-authored cubic curve, nosing along.
    SpritePath glider{.nodes = {{.move   = SpritePathMove::onCurve(gliderCurve),
                                 .pacing = PathPacing::speed(38.0f),
                                 .facing = FacingPolicy::RotateToFacing}},
                      .start = {0, 0}};

    std::vector<SpritePath*> all{&walker, &fish, &shuttle, &tumbler, &swinger, &glider};
    bool                     paused = false;
    std::chrono::nanoseconds seekAt{0};

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::PlayPause)) {
            paused = !paused;
            for (SpritePath* m : all) paused ? m->pause() : m->play();
            std::printf("[dev] %s\n", paused ? "paused" : "playing");
        }
        if (in.justPressed(Action::Restart)) {
            for (SpritePath* m : all) m->restart();
            seekAt = 0ns;
            paused = false;
            std::printf("[dev] restarted\n");
        }
        if (in.justPressed(Action::Stop)) {
            for (SpritePath* m : all) m->stop();
            paused = true;
            std::printf("[dev] stopped\n");
        }
        if (in.justPressed(Action::SeekForward)) {
            seekAt += 1s;
            for (SpritePath* m : all) m->seek(seekAt);
            std::printf("[dev] seek +1s (%lld ms)\n", static_cast<long long>(seekAt / 1ms));
        }
        if (in.justPressed(Action::SeekBack)) {
            seekAt = seekAt > 1s ? seekAt - 1s : 0ns;
            for (SpritePath* m : all) m->seek(seekAt);
            std::printf("[dev] seek -1s (%lld ms)\n", static_cast<long long>(seekAt / 1ms));
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());

        for (SpritePath* m : all) m->advance();  // bare advance() loops each mover
    });

    std::vector<Sprite> movers;
    FrameDrawState      frame;
    loop.renderLoop([&]() {
        movers.clear();

        // 7. Ghost twin — read the walker's RAW sample (position + current frame) and draw the SAME
        //    silhouette in a translucent slate palette, offset down-right. Submitted FIRST so it sits under
        //    the walker. This is the raw sample() read beside the walker's applyTo write.
        Sprite shadowS{.key = "walkerShadow"};
        if (const AnimationFrame* f = walker.frame(); f && f->hasArt()) {
            shadowS.atlas = f->atlas();
            shadowS.tile  = f->tile();
            shadowS.size  = f->size();
        }
        shadowS.palette = shadowPalId;
        shadowS.alpha   = 0.6f;
        shadowS.x       = static_cast<int>(std::lround(walker.position().x)) + 4;
        shadowS.y       = static_cast<int>(std::lround(walker.position().y)) + 4;
        movers.push_back(shadowS);

        // 1. Walker — applyTo writes the frame art, position, and flip (drawn over its shadow).
        Sprite walkerS{.key = "walker"};
        walker.applyTo(walkerS);
        movers.push_back(walkerS);

        // The arrow / block movers — set art first (no animation track), then applyTo writes position (+ any
        // transform / flip the node declares).
        const auto arrow = [&](SpritePath& m, PaletteId pal, const char* key) {
            Sprite s{.key = key};
            s.atlas   = arrowAtlas;
            s.palette = pal;
            s.tile    = 0;
            s.size    = AssetDimensions::GameBoy8x8;
            m.applyTo(s);
            return s;
        };
        const auto block = [&](SpritePath& m, PaletteId pal, const char* key) {
            Sprite s{.key = key};
            s.atlas   = blockAtlas;
            s.palette = pal;
            s.tile    = 0;
            s.size    = AssetDimensions::GameBoy8x8;
            m.applyTo(s);
            return s;
        };

        movers.push_back(arrow(fish, fishPalId, "fish"));
        movers.push_back(arrow(shuttle, shuttlePalId, "shuttle"));
        movers.push_back(block(tumbler, tumblerPalId, "tumbler"));
        movers.push_back(block(swinger, swingerPalId, "swinger"));
        movers.push_back(arrow(glider, gliderPalId, "glider"));

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

    std::printf("sprite path demo — seven movers: walker (walk-cycle + flip patrol), fish (eased loop, noses "
                "along), shuttle (Hermite arc, reverses), tumbler (spin + breathe), swinger (corner pivot), "
                "glider (pre-authored curve), and the walker's raw-sample ghost twin. A play/pause, B restart, "
                "Start stop, arrows seek ±1s, Select fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
