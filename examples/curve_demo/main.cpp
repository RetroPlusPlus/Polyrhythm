// Curve demo — a runnable host that VISUALLY proves the Curve primitive's queries. It opens a window and
// plots, with small palette-colored sprite markers (a tiny in-memory atlas — no committed asset), three
// authored curves over a dim background grid in the 160×144 viewport:
//
//   • a CATMULL-ROM curve through gold waypoints (the curve passes THROUGH every waypoint), with cyan
//     dots sampled by Curve::at(t) tracing the smooth shape, a PINK walker advanced by
//     Curve::atDistance(s) at CONSTANT SPEED (even spacing — it does not lurch through the control
//     clusters a raw-parameter walk would), and GREEN tangent ticks from Curve::tangentAtDistance — the
//     facing at the walker's own arc-length point.
//   • a HERMITE curve that LEAVES and ARRIVES along its supplied tangents.
//   • a funky CLOSED curve (magenta) whose interior is a curve-DEFINED REGION: its boundary is sampled
//     into the polygon the engine's region gate consumes, and a gentle radial ripple is confined to it,
//     so the background shimmers only inside the curve's outline. (The ripple effect itself is untouched;
//     the engine confines it to the shape — the curve just supplies the shape.) Z toggles the ripple
//     between the curve region and the whole frame so the confinement is unmistakable.
//
// This is the visual sanity check for a pure-CPU primitive — the device-free ctest suite is the real
// gate. X restarts the walker; close to quit.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

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
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;   // 20×18 tiles cover the 160×144 viewport

// One marker role per palette — the color each marker draws through (uploaded below in this order).
enum Pal : std::uint8_t { kSample = 0, kWaypoint = 1, kWalker = 2, kTangent = 3, kBlob = 4 };

// The demo's input vocabulary: the two toggles plus a dev key.
enum class Action : std::uint8_t { RestartWalker, CycleRipple, Fullscreen };

// One centered 8×8 marker at (x, y), drawing from `atlas` through `palette`.
Sprite marker(float x, float y, AtlasId atlas, PaletteId palette) {
    Sprite s{.key = "marker"};  // placeholder key; the caller assigns a unique per-index key
    s.x       = static_cast<int>(std::lround(x)) - 4;  // center the 8×8 cell on the point
    s.y       = static_cast<int>(std::lround(y)) - 4;
    s.size    = AssetDimensions::GameBoy8x8;
    s.tile    = 0;
    s.atlas   = atlas;
    s.palette = palette;
    return s;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Curve Demo"},
        .window = {.title = "Polyrhythm — curve demo (curves + curve-defined region)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::RestartWalker, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::CycleRipple,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen,    {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A tiny 8×8 marker atlas: every texel is palette-index 1 (a solid square); the color comes from the
    // selected palette's entry [1]. (Atlas index 0 would be the OBJ-transparent hole; we use index 1.)
    std::array<std::uint8_t, 64> markerArt{};
    markerArt.fill(1);
    const AtlasId markerAtlas = renderer.uploadAtlas(markerArt.data(), 8, 8).atlasId;

    // A dim 8×8 background grid tile (index 2 on the cell border, index 1 inside) so the ripple has
    // something to displace; kept low-contrast so the bright curve markers still read on top.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;

    // One palette per marker role; entry [1] is the visible color (entry [0] is unused — never sampled).
    const std::array<Rgba8, 2> samplePal{{{0, 0, 0}, {90, 200, 255}}};   // cyan — Curve::at samples
    const std::array<Rgba8, 2> wayPal{{{0, 0, 0}, {255, 210, 90}}};      // gold — waypoints
    const std::array<Rgba8, 2> walkPal{{{0, 0, 0}, {255, 90, 120}}};     // pink — constant-speed walker
    const std::array<Rgba8, 2> tanPal{{{0, 0, 0}, {150, 255, 150}}};     // green — tangent ticks
    const std::array<Rgba8, 2> blobPal{{{0, 0, 0}, {235, 120, 255}}};    // magenta — funky region outline
    const std::array<PaletteId, 5> markerPalettes{
        renderer.uploadPalette(std::span<const Rgba8>(samplePal)),
        renderer.uploadPalette(std::span<const Rgba8>(wayPal)),
        renderer.uploadPalette(std::span<const Rgba8>(walkPal)),
        renderer.uploadPalette(std::span<const Rgba8>(tanPal)),
        renderer.uploadPalette(std::span<const Rgba8>(blobPal))};
    // Resolve a marker ROLE (a Pal enumerator) to its uploaded palette, then build the centered marker that
    // draws from the marker atlas through it — each sprite now names its own sheet + palette directly.
    const auto mark = [&](float x, float y, Pal role) {
        return marker(x, y, markerAtlas, markerPalettes[role]);
    };

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {20, 26, 40}, {40, 52, 78}}};  // dim navy grid
    const PaletteId            gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // ── The three authored curves ────────────────────────────────────────────────────────────────
    const std::array<Vec2, 5> waypoints{{{18, 34}, {54, 20}, {92, 44}, {128, 22}, {150, 40}}};
    const Curve catmull = Curve::throughPoints(std::span<const Vec2>(waypoints), false);
    const Curve hermiteC =
        Curve::hermite(Vec2{14, 122}, Vec2{40, -54}, Vec2{74, 122}, Vec2{40, 54});

    // A funky CLOSED curve — a lumpy blob. Catmull-Rom through wobbly points, wrapped into a loop, so the
    // boundary is genuinely curved (not a straight-edged polygon).
    const std::array<Vec2, 6> blobPts{{{116, 70}, {136, 86}, {128, 110}, {106, 118}, {90, 100}, {100, 78}}};
    const Curve blob = Curve::throughPoints(std::span<const Vec2>(blobPts), true);

    // Sample the closed curve's boundary into the polygon the engine's region gate consumes (≤ 64
    // vertices). The region is therefore CURVE-DEFINED: its outline is the curve, sampled. Built once —
    // the blob does not change.
    constexpr int      kRegionSamples = 48;
    std::vector<Point> blobRegionPts;
    blobRegionPts.reserve(kRegionSamples);
    for (int i = 0; i < kRegionSamples; ++i) {
        const Vec2 p = blob.at(static_cast<float>(i) / static_cast<float>(kRegionSamples));
        blobRegionPts.push_back(Point{p.x, p.y});
    }
    const ShapePoints blobRegion{.points = blobRegionPts};

    // Bake the catmull curve's arc-length table ONCE — the walker queries it every frame with no
    // per-call resample (the reuse path; the Curve's own atDistance would rebuild the table each call).
    const ArcLengthTable catmullArc   = catmull.arcTable();
    const float          walkerLength = catmullArc.length();
    float                walkerDist   = 0.0f;  // arc-length cursor; advances at constant speed, wraps

    // Z cycles the ripple: 0 = confined to the curve region, 1 = whole frame, 2 = OFF (no effect at all —
    // the raw scene, so the magenta outline reads as a hollow ring over plain grid, nothing filled inside).
    int        rippleMode = 0;
    const auto rippleLabel = [](int m) {
        return m == 0 ? "confined to the curve region" : (m == 1 ? "whole frame" : "OFF (raw scene)");
    };

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::RestartWalker)) {
            walkerDist = 0.0f;
            std::printf("[dev] walker restarted\n");
        }
        if (in.justPressed(Action::CycleRipple)) {
            rippleMode = (rippleMode + 1) % 3;
            std::printf("[dev] ripple: %s\n", rippleLabel(rippleMode));
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        walkerDist += 0.24f;
        if (walkerLength > 0.0f && walkerDist > walkerLength) walkerDist -= walkerLength;
    });

    std::vector<Sprite> sprites;
    static const std::vector<std::string> sprKeys =
        [] { std::vector<std::string> v; for (int k = 0; k < 512; ++k) v.push_back("m" + std::to_string(k)); return v; }();
    FrameDrawState      frame;
    loop.renderLoop([&]() {
        sprites.clear();

        // Catmull-Rom: Curve::at samples (cyan) + waypoints (gold).
        constexpr int kSamples = 48;
        for (int i = 0; i <= kSamples; ++i) {
            const Vec2 p = catmull.at(static_cast<float>(i) / static_cast<float>(kSamples));
            sprites.push_back(mark(p.x, p.y, kSample));
        }
        for (const Vec2& w : waypoints) sprites.push_back(mark(w.x, w.y, kWaypoint));

        // Hermite curve sampled (cyan) — its ends leave/arrive along the supplied tangents.
        for (int i = 0; i <= kSamples; ++i) {
            const Vec2 p = hermiteC.at(static_cast<float>(i) / static_cast<float>(kSamples));
            sprites.push_back(mark(p.x, p.y, kSample));
        }

        // The funky closed curve's outline (magenta) — the boundary of the curve-defined region below.
        for (int i = 0; i < kRegionSamples; ++i) {
            const Vec2 p = blob.at(static_cast<float>(i) / static_cast<float>(kRegionSamples));
            sprites.push_back(mark(p.x, p.y, kBlob));
        }

        // The constant-speed walker (pink) + a few tangent ticks (green) ahead of it along travel. The
        // facing comes from tangentAtDistance — the heading at the SAME arc-length point as the walker,
        // not tangent(s / length), which would read a different point on this non-uniform curve.
        const Vec2 here = catmullArc.atDistance(walkerDist);
        const Vec2 dir  = catmullArc.tangentAtDistance(walkerDist);
        sprites.push_back(mark(here.x, here.y, kWalker));
        for (int k = 1; k <= 3; ++k) {
            sprites.push_back(mark(here.x + dir.x * (4.0f * k), here.y + dir.y * (4.0f * k), kTangent));
        }

        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        DrawLayer markers{.key = "curveMarkers"};
        markers.z       = 0;
        markers.size    = PixelSize{kViewW, kViewH};
        for (std::size_t i = 0; i < sprites.size(); ++i) sprites[i].key = sprKeys[i];  // assign unique keys
        markers.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(markers));

        // A gentle radial ripple. Mode 0 confines it to the CURVE-DEFINED region (the sampled blob
        // boundary) — the engine masks the effect to the shape, the curve supplies the shape. Mode 1 drops
        // the region so the SAME effect runs whole-frame. Mode 2 emits no effect at all: the raw scene, so
        // the magenta outline is plainly a hollow ring over flat grid — nothing is filled inside it.
        frame.postEffects.clear();
        frame.regions.clear();
        if (rippleMode != 2) {
            ScreenSpaceEffect ripple{
                .kind      = ScreenSpaceEffectKind::Ripple,
                .amplitude = 3.5f,
                .frequency = 5.0f,
                .phase     = static_cast<float>(tick) * 0.01f,  // ~0.6 cycles/s — rings drift out slowly
                .center    = Point{113, 92},                     // roughly the blob's center
                .decay     = 2.0f};
            if (rippleMode == 0)  // SAME effect, now bounded by the curve (a region the frame owns)
                frame.regions.push_back(Region{.key = "blobRip", .shape = blobRegion, .effects = {ripple}});
            else
                frame.postEffects.push_back(ripple);  // mode 1: whole-frame
        }

        renderer.renderFrame(frame);
    });

    std::printf("curve demo — gold = waypoints, cyan = Curve::at samples, pink = the constant-speed "
                "atDistance walker, green = tangentAtDistance ticks, magenta = a closed curve whose "
                "interior ripples (a curve-defined region). X restarts the walker, Z cycles the ripple "
                "(curve region → whole frame → OFF), Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
