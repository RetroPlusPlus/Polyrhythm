// Curve-region demo — a runnable host that VISUALLY proves a curved effect-region BOUNDARY. It opens a
// window and confines a gentle radial ripple of a dim background grid to two regions side by side, both
// the SAME rounded quadratic-Bezier outline:
//
//   • LEFT  — an ANALYTIC quadratic-curved boundary (ShapePoints::fromCurve). The boundary is the curve
//     itself: the rippling area's edge is smooth, exact between the four quadratic control points, with no
//     facets at any zoom.
//   • RIGHT — the SAME outline SAMPLED to a coarse polygon (a handful of straight edges). The rippling
//     area's edge is visibly faceted — the straight-edge approximation a sampled boundary gives.
//
// The same effect is confined to each; only the boundary representation differs, so the no-facets win of
// the analytic gate reads directly against the sampled one. A magenta outline traces each boundary (the
// analytic curve sampled densely on the left; the actual coarse vertices on the right).
//
// Containment is the device-free ctest suite's job (sdCurveAnalytic vs Curve::signedDistance); this is the
// live GPU sanity check. Z toggles the right side between coarse and fine sampling so the facets-vs-smooth
// contrast is unmistakable; X toggles fill vs STROKE — confining the ripple to a band along the boundary (a
// curved hoop / outline) instead of the filled interior; Backspace = fullscreen; close to quit.

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
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// A closed rounded outline of four quadratic Bezier segments about (cx,cy) with axis radius r: on-curve
// points at N/E/S/W, controls at the diagonal corners. Genuinely quadratic — its distance has a closed
// form, so the analytic gate renders its boundary exactly.
[[nodiscard]] Curve roundedQuad(float cx, float cy, float r) {
    const Vec2 n{cx, cy - r}, e{cx + r, cy}, s{cx, cy + r}, w{cx - r, cy};
    const Vec2 ne{cx + r, cy - r}, se{cx + r, cy + r}, sw{cx - r, cy + r}, nw{cx - r, cy - r};
    Curve c;
    c.closed   = true;
    c.segments = {CurveSegment{n, ne, e, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{e, se, s, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{s, sw, w, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{w, nw, n, Vec2{}, CurveDegree::Quadratic}};
    return c;
}

// One centered 8×8 marker at (x, y), drawing from `atlas` through `palette`.
[[nodiscard]] Sprite marker(float x, float y, AtlasId atlas, PaletteId palette) {
    Sprite s{.key = "marker"};  // placeholder key; the caller assigns a unique per-index key
    s.x       = static_cast<int>(std::lround(x)) - 4;
    s.y       = static_cast<int>(std::lround(y)) - 4;
    s.size    = AssetDimensions::GameBoy8x8;
    s.tile    = 0;
    s.atlas   = atlas;
    s.palette = palette;
    return s;
}

enum Pal : std::uint8_t { kOutline = 0, kVertex = 1 };

// The demo's input vocabulary: the two toggles plus a dev key.
enum class Action : std::uint8_t { ToggleStroke, ToggleFacets, Fullscreen };

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Curve Region Demo"},
        .window = {.title = "Polyrhythm — curve-region demo (analytic vs sampled boundary)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ToggleStroke, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::ToggleFacets, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen,   {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A tiny 8×8 marker atlas (every texel is palette-index 1, a solid square) for the boundary outlines.
    std::array<std::uint8_t, 64> markerArt{};
    markerArt.fill(1);
    const AtlasId markerAtlas = renderer.uploadAtlas(markerArt.data(), 8, 8).atlasId;

    // A dim grid tile so the confined ripple has something to distort; low-contrast so the outlines read.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;

    const std::array<Rgba8, 2> outlinePal{{{0, 0, 0}, {235, 120, 255}}};  // magenta — boundary outline
    const std::array<Rgba8, 2> vertexPal{{{0, 0, 0}, {255, 210, 90}}};    // gold — coarse sample vertices
    const std::array<PaletteId, 2> markerPalettes{renderer.uploadPalette(std::span<const Rgba8>(outlinePal)),
                                                  renderer.uploadPalette(std::span<const Rgba8>(vertexPal))};
    // Resolve a marker ROLE (kOutline / kVertex) to its uploaded palette and build the marker that draws
    // from the marker atlas through it — each sprite names its own sheet + palette directly.
    const auto mark = [&](float x, float y, Pal role) {
        return marker(x, y, markerAtlas, markerPalettes[role]);
    };

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {20, 26, 40}, {40, 52, 78}}};  // dim navy grid
    const PaletteId            gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // The same rounded outline on each side; left is the analytic curve, right is sampled to a polygon.
    constexpr float    kRadius = 30.0f;
    const Vec2         leftC{44, 80}, rightC{116, 80};
    const Curve        leftCurve  = roundedQuad(leftC.x, leftC.y, kRadius);
    const Curve        rightCurve = roundedQuad(rightC.x, rightC.y, kRadius);
    const ShapePoints  leftRegion = ShapePoints::fromCurve(leftCurve);  // the boundary IS the curve

    // Sample the right outline to a coarse polygon — the straight-edge approximation a sampled boundary
    // gives. Z switches between coarse (visible facets) and fine (nearly smooth) sample counts.
    const auto sampleRegion = [&](const Curve& c, int n) {
        ShapePoints r;
        r.points.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const Vec2 p = c.at(static_cast<float>(i) / static_cast<float>(n));
            r.points.push_back(Point{p.x, p.y});
        }
        return r;
    };
    constexpr int kCoarse = 9, kFine = 48;
    bool          fine    = false;  // Z toggles the right side coarse/fine
    bool          stroked = false;  // X toggles fill <-> stroke (confine the ripple to the boundary band)

    int tick = 0;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::ToggleFacets)) {
            fine = !fine;
            std::printf("[dev] right boundary samples: %d\n", fine ? kFine : kCoarse);
        }
        if (in.justPressed(Action::ToggleStroke)) {
            stroked = !stroked;
            std::printf("[dev] shape mode: %s\n", stroked ? "stroke (boundary band)" : "fill (interior)");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        ++tick;
    });

    std::vector<Sprite> sprites;
    // Stable per-marker keys (required + unique frame-wide) indexed by position in `sprites`, built once.
    static const std::vector<std::string> sprKeys =
        [] { std::vector<std::string> v; for (int k = 0; k < 512; ++k) v.push_back("m" + std::to_string(k)); return v; }();
    FrameDrawState      frame;
    loop.renderLoop([&]() {
        ShapePoints rightRegion = sampleRegion(rightCurve, fine ? kFine : kCoarse);
        // X: confine the ripple to a band along the boundary (a curved hoop) instead of the filled interior.
        constexpr float   kStrokePx         = 10.0f;
        const float       strokeW           = stroked ? kStrokePx : 0.0f;
        ShapePoints       leftRegionStroked = leftRegion;
        leftRegionStroked.strokeWidth       = strokeW;
        rightRegion.strokeWidth             = strokeW;

        // Outline both boundaries: the analytic curve sampled densely (left), the actual polygon vertices
        // (right) — gold so the facet corners read.
        sprites.clear();
        constexpr int kOutlineSamples = 64;
        for (int i = 0; i < kOutlineSamples; ++i) {
            const Vec2 p = leftCurve.at(static_cast<float>(i) / static_cast<float>(kOutlineSamples));
            sprites.push_back(mark(p.x, p.y, kOutline));
        }
        for (const Point& p : rightRegion.points) sprites.push_back(mark(p.x, p.y, kVertex));
        for (std::size_t i = 0; i < sprites.size(); ++i) sprites[i].key = sprKeys[i];  // assign unique keys

        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        DrawLayer outlines{.key = "boundaryOutlines"};
        outlines.z       = 0;
        outlines.size    = PixelSize{kViewW, kViewH};
        outlines.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(outlines));

        // The SAME gentle ripple confined to each region: left bounded by the analytic curve, right by the
        // sampled polygon. Only the boundary differs — the analytic edge is smooth, the sampled one facets.
        const float phase = static_cast<float>(tick) * 0.01f;  // ~0.6 cycles/s — slow
        ScreenSpaceEffect rippleLeft{.kind      = ScreenSpaceEffectKind::Ripple,
                                     .amplitude = 3.0f,
                                     .frequency = 5.0f,
                                     .phase     = phase,
                                     .center    = Point{leftC.x, leftC.y},
                                     .decay     = 2.0f};
        ScreenSpaceEffect rippleRight = rippleLeft;
        rippleRight.center = Point{rightC.x, rightC.y};

        frame.regions.clear();
        frame.regions.push_back(Region{.key = "leftRip",  .shape = leftRegionStroked, .effects = {rippleLeft}});  // analytic curve boundary
        frame.regions.push_back(Region{.key = "rightRip", .shape = rightRegion, .effects = {rippleRight}});       // sampled polygon boundary

        renderer.renderFrame(frame);
    });

    std::printf("curve-region demo — LEFT: a ripple confined to an ANALYTIC quadratic-curved boundary "
                "(smooth edge, no facets). RIGHT: the SAME outline sampled to a coarse polygon (faceted "
                "edge). Z toggles the right side coarse/fine; X toggles fill vs STROKE (the ripple confines "
                "to a band along the boundary — a curved hoop); Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
