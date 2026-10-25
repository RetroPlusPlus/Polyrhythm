// Path-walker demo — a runnable host that VISUALLY proves PathWalker: it opens a window and sends THREE
// movers along ONE Catmull-Rom curve at once, each driven by a different PathPacing form, over a dim
// background grid in the 160×144 viewport:
//
//   • a CYAN arrow paced by PathPacing::speed(...) — constant speed, even spacing along the arc.
//   • a GOLD arrow paced by PathPacing::eased(..., InOutQuad) on the SAME curve — visibly slow at the ends
//     and quick through the middle, so the easing reads against the constant-speed mover beside it.
//   • a PINK arrow paced by PathPacing::distanceTween(...) — a multi-segment distance profile that runs to
//     the end, BACKS UP partway, then continues, the motion only the Tween-driven form can express.
//
// Every arrow is oriented from ITS walker's facing (a per-sprite Transform rotation from
// atan2(facing.y, facing.x)), so a single walker drives both the position AND the heading. Small cyan dots
// trace the curve itself (Curve::at samples) so the shared path is visible.
//
// This is the visual sanity check for a pure-CPU layer — the device-free ctest suite is the real gate.
// A restarts all three; Select toggles fullscreen; close to quit.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/path_walker.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// The demo's input vocabulary: restart the movers and toggle fullscreen.
enum class Action : std::uint8_t { Restart, Fullscreen };

// An 8×8 right-pointing arrow (index 1 = body, index 0 = the OBJ-transparent hole); at rotation 0 it points
// toward +x, so a Transform rotation by the walker's heading aims it along travel.
constexpr const char* kArrowArt[8] = {
    "........",
    "...##...",
    "...###..",
    "#######.",
    "########",
    "#######.",
    "...###..",
    "...##...",
};

// The heading (a unit facing vector) as a clockwise degree angle in the engine's top-left-origin pixel
// space — exactly what Transform::rotation expects, so atan2(y, x) maps straight through.
float headingDegrees(Vec2 facing) {
    return std::atan2(facing.y, facing.x) * 57.29577951308232f;  // 180 / π
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Path Walker Demo"},
        .window = {.title = "Polyrhythm — path walker (three movers on one curve)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Restart,    {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // The arrow atlas (index 1 = body). Uploaded once; each mover draws it through its own palette.
    std::array<std::uint8_t, 64> arrowArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) arrowArt[static_cast<std::size_t>(y) * 8 + x] = kArrowArt[y][x] == '#' ? 1 : 0;
    const AtlasId arrowAtlas = renderer.uploadAtlas(arrowArt.data(), 8, 8).atlasId;

    // A tiny solid 8×8 marker (every texel index 1) for the curve trace dots.
    std::array<std::uint8_t, 64> dotArt{};
    dotArt.fill(1);
    const AtlasId dotAtlas = renderer.uploadAtlas(dotArt.data(), 8, 8).atlasId;

    // A dim grid tile (index 2 on the border, index 1 inside) so the movers have a backdrop to read against.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;

    // One palette per role; entry [1] is the visible color (entry [0] is unused — never sampled).
    const std::array<Rgba8, 2> speedPal{{{0, 0, 0}, {90, 200, 255}}};   // cyan — constant speed
    const std::array<Rgba8, 2> easedPal{{{0, 0, 0}, {255, 210, 90}}};   // gold — eased
    const std::array<Rgba8, 2> tweenPal{{{0, 0, 0}, {255, 90, 150}}};   // pink — distance-tween (reverses)
    const std::array<Rgba8, 2> dotPal{{{0, 0, 0}, {70, 90, 130}}};      // dim blue-gray — the path trace
    const PaletteId speedPalId = renderer.uploadPalette(std::span<const Rgba8>(speedPal));
    const PaletteId easedPalId = renderer.uploadPalette(std::span<const Rgba8>(easedPal));
    const PaletteId tweenPalId = renderer.uploadPalette(std::span<const Rgba8>(tweenPal));
    const PaletteId dotPalId   = renderer.uploadPalette(std::span<const Rgba8>(dotPal));

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {18, 22, 34}, {34, 44, 66}}};  // dim navy grid
    const PaletteId            gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell> gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // ── The shared curve — a Catmull-Rom wave the three movers all walk ──────────────────────────────
    const std::array<Vec2, 5> waypoints{{{16, 110}, {50, 40}, {90, 112}, {124, 40}, {150, 100}}};
    const Curve          path = Curve::throughPoints(std::span<const Vec2>(waypoints), false);
    const ArcLengthTable arc  = path.arcTable();  // bake ONCE — every walker holds a copy by value
    const float          len  = arc.length();

    // The Tween the distance-tween mover is driven by — game-owned, must OUTLIVE the walker. It runs to the
    // end, backs up to 40% of the path, then continues to the end: motion only the raw distance form opens.
    const Tween<float> distanceProfile = Tween<float>::of(0.0f, len, std::chrono::seconds{5})
                                             .then(len * 0.4f, std::chrono::seconds{2})
                                             .then(len, std::chrono::seconds{3});

    // The three movers — all on the same baked table, each a different pacing form.
    PathWalker speedMover{.table = arc, .pacing = PathPacing::speed(len / 13.0f)};
    PathWalker easedMover{
        .table = arc, .pacing = PathPacing::eased(std::chrono::seconds{10}, Easing::InOutQuad)};
    PathWalker tweenMover{.table = arc, .pacing = PathPacing::distanceTween(distanceProfile)};

    // Advance on the sim tick (not per render frame) so motion speed is display-refresh-independent.
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Restart)) {
            speedMover.restart();
            easedMover.restart();
            tweenMover.restart();
            std::printf("[dev] movers restarted\n");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        speedMover.advance();  // bare advance() loops (PlaybackMode::loopIndefinitely)
        easedMover.advance();
        tweenMover.advance();
    });

    // Stable per-dot keys for the curve trace (the required-ObjectKey identity model).
    constexpr int kTrace = 60;
    static const std::vector<std::string> dotKeys = [] {
        std::vector<std::string> v;
        for (int i = 0; i <= kTrace; ++i) v.push_back("dot" + std::to_string(i));
        return v;
    }();

    // Build one oriented arrow sprite for a mover: centered on its position, rotated to its facing.
    const auto arrowFor = [&](const PathWalker& w, AtlasId atlas, PaletteId palette, const char* key) {
        Sprite s{.key = key};
        s.x         = static_cast<int>(std::lround(w.position().x)) - 4;  // center the 8×8 cell
        s.y         = static_cast<int>(std::lround(w.position().y)) - 4;
        s.size      = AssetDimensions::GameBoy8x8;
        s.tile      = 0;
        s.atlas     = atlas;
        s.palette   = palette;
        s.transform = Transform::rotation(headingDegrees(w.facing()), 4.0f, 4.0f);  // about the sprite center
        return s;
    };

    std::vector<Sprite> traceDots;
    std::vector<Sprite> arrows;
    FrameDrawState      frame;
    loop.renderLoop([&]() {
        // The curve trace — small dim dots along the shared path.
        traceDots.clear();
        for (int i = 0; i <= kTrace; ++i) {
            const Vec2 p = path.at(static_cast<float>(i) / static_cast<float>(kTrace));
            Sprite     d{.key = dotKeys[static_cast<std::size_t>(i)]};
            d.x       = static_cast<int>(std::lround(p.x)) - 4;
            d.y       = static_cast<int>(std::lround(p.y)) - 4;
            d.size    = AssetDimensions::GameBoy8x8;
            d.tile    = 0;
            d.atlas   = dotAtlas;
            d.palette = dotPalId;
            traceDots.push_back(d);
        }

        // The three oriented movers.
        arrows.clear();
        arrows.push_back(arrowFor(speedMover, arrowAtlas, speedPalId, "speedMover"));
        arrows.push_back(arrowFor(easedMover, arrowAtlas, easedPalId, "easedMover"));
        arrows.push_back(arrowFor(tweenMover, arrowAtlas, tweenPalId, "tweenMover"));

        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        DrawLayer trace{.key = "curveTrace"};
        trace.z       = 0;
        trace.size    = PixelSize{kViewW, kViewH};
        trace.content = SpriteContent{.sprites = std::span<const Sprite>(traceDots)};
        frame.layers.push_back(trace);

        DrawLayer movers{.key = "movers"};
        movers.z       = 10;
        movers.size    = PixelSize{kViewW, kViewH};
        movers.content = SpriteContent{.sprites = std::span<const Sprite>(arrows)};
        frame.layers.push_back(std::move(movers));

        renderer.renderFrame(frame);
    });

    std::printf("path walker demo — one curve, three movers: cyan = constant speed, gold = eased "
                "(slow at the ends), pink = distance-tween (runs to the end, backs up, continues). Each "
                "arrow points along its own heading. A restarts all three, Select = fullscreen. Close to "
                "quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
