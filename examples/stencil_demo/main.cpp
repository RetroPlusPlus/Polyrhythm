// Stencil demo — a runnable host that VISUALLY proves the built-in Stencil effect: a TRANSPARENCY along a
// shape. Nothing is erased or destroyed — a region of a layer is made SEE-THROUGH so what's behind it shows
// through, and the shape's two sides stay live, effect-able regions. It opens a window with three layers —
// a vivid REAR scene (z low), a brick WALL (z high) made see-through by the stencil() helper, and the
// backdrop behind everything — and makes the wall see-through along a slowly-drifting shape:
//
//   • InsideTransparent  — the wall goes see-through INSIDE the shape; the rest stays solid. (default)
//   • OutsideTransparent — the wall goes see-through OUTSIDE the shape; only the inside stays solid.
//
// What shows through depends on the effect's SCOPE — the same transparency, two reveal targets:
//   • Layer scope — only the WALL is see-through there → the REAR scene behind it shows through.
//   • Below scope — the wall AND everything beneath go see-through → the BACKDROP shows through.
//
// Both sides of the shape are regions that carry effects, passed to stencil() as its inside / outside effect
// lists: a gentle ripple plays in the inside region and a slow wave in the outside region, so a see-through
// area is shown to carry an effect just like a solid one.
//
// A toggles which side is see-through; B cycles the shape (circle / capsule / triangle / rectangle /
// roundedRectangle / hexagon / quadratic curve); Up toggles a soft FEATHERED edge against a hard one; Down
// toggles the scope (reveal the rear layer vs reveal the backdrop); Start toggles the inside/outside region
// effects on/off (off = a plain see-through); Right swaps the inside/outside effects; Select = fullscreen;
// close to quit.
//
// Containment + coverage are the device-free ctest suite's job (stencilCoverage / stencilSurvival vs the
// region SDF); this is the live GPU sanity check.

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
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// The demo's input vocabulary: which side is see-through, the shape cycle, the feathered edge, the
// effect scope, the region effects on/off, the inside/outside swap, and fullscreen.
enum class Action : std::uint8_t {
    ToggleSide, CycleShape, ToggleFeather, ToggleScope, ToggleEffects, SwapEffects, Fullscreen,
};

// A closed SCALLOPED FLOWER of `lobes` quadratic Bezier segments about (cx,cy): anchor points sit on the
// inner radius rIn, each segment's control point bulges out to rOut between them, so the boundary is an
// unmistakably CURVED outline (petals) — not a circle, not a polygon. Genuinely quadratic, so the analytic
// stencil shader (region_stencil_curve.frag) makes the layer see-through along the true curve exactly, no facets.
[[nodiscard]] Curve flowerCurve(float cx, float cy, float rIn, float rOut, int lobes) {
    constexpr float kTwoPi = 6.283185307179586f;
    Curve c;
    c.closed = true;
    const auto pt = [&](float radius, float frac) {
        const float a = kTwoPi * frac;
        return Vec2{cx + radius * std::cos(a), cy + radius * std::sin(a)};
    };
    for (int i = 0; i < lobes; ++i) {
        const Vec2 p0   = pt(rIn,  static_cast<float>(i) / static_cast<float>(lobes));
        const Vec2 ctrl = pt(rOut, (static_cast<float>(i) + 0.5f) / static_cast<float>(lobes));  // bulge out
        const Vec2 p2   = pt(rIn,  static_cast<float>(i + 1) / static_cast<float>(lobes));
        c.segments.push_back(CurveSegment{p0, ctrl, p2, Vec2{}, CurveDegree::Quadratic});
    }
    return c;
}

// The base region for a cycle index, centered on the screen (80, 72); the render loop adds the drift via a
// transform translation, so one offset moves every shape (including the curve).
[[nodiscard]] ShapePoints shapeForIndex(int i) {
    switch (i % 7) {
        case 0:  return ShapePoints::circle({80, 72}, 38);
        case 1:  return ShapePoints::capsule({52, 72}, {108, 72}, 26);
        case 2:  return ShapePoints::triangle({80, 32}, {124, 110}, {36, 110});
        case 3:  return ShapePoints::rectangle({44, 44}, 72, 56);
        case 4:  return ShapePoints::roundedRectangle({40, 40}, 80, 64, 18);
        case 5:  return ShapePoints::regularPolygon({80, 72}, 42, 6);            // hexagon
        default: return ShapePoints::fromCurve(flowerCurve(80, 72, 26, 46, 6));  // scalloped quadratic curve
    }
}
[[nodiscard]] const char* shapeName(int i) {
    switch (i % 7) {
        case 0: return "circle";           case 1: return "capsule";
        case 2: return "triangle";         case 3: return "rectangle";
        case 4: return "roundedRectangle"; case 5: return "regularPolygon (hexagon)";
        default: return "quadratic curve (scalloped flower)";
    }
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Stencil Demo"},
        .window = {.title = "Polyrhythm — stencil demo (region see-through: hole / porthole)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ToggleSide,    {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::CycleShape,    {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::ToggleFeather, {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::ToggleScope,   {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
        {Action::ToggleEffects, {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::SwapEffects,   {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {Action::Fullscreen,    {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // Rear scene: one solid 8×8 tile (palette index 1); the color comes from the per-cell palette
    // selection, so one tile draws a multi-color scene.
    std::array<std::uint8_t, 64> solidArt{};
    solidArt.fill(1);
    const AtlasId rearAtlas = renderer.uploadAtlas(solidArt.data(), 8, 8).atlasId;

    // Wall: a two-tile brick atlas (16×8). Tile 0 carries its vertical mortar seam at column 0, tile 1 at
    // column 4; alternating them per row makes a running-bond brick wall, so the foreground reads as a
    // distinct TEXTURED surface, not a flat fill. index 1 = brick, index 2 = mortar.
    std::array<std::uint8_t, 128> brickArt{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int  tile   = x / 8;
            const int  localX = x % 8;
            const bool mortar = (y == 7) || (tile == 0 ? localX == 0 : localX == 4);  // course + seam lines
            brickArt[static_cast<std::size_t>(y) * 16 + x] = mortar ? 2 : 1;
        }
    }
    const AtlasId wallAtlas = renderer.uploadAtlas(brickArt.data(), 16, 8).atlasId;

    // Rear-scene palette set: four bright hues a cell selects between, so the revealed scene is an
    // unmistakable distinct layer behind the wall. Index 0 is unused backdrop black.
    const std::array<Rgba8, 2> palCyan{{{0, 0, 0}, {70, 200, 220}}};
    const std::array<Rgba8, 2> palGold{{{0, 0, 0}, {235, 190, 70}}};
    const std::array<Rgba8, 2> palRose{{{0, 0, 0}, {225, 95, 150}}};
    const std::array<Rgba8, 2> palLime{{{0, 0, 0}, {130, 210, 100}}};
    const std::array<PaletteId, 4> rearSet{renderer.uploadPalette(std::span<const Rgba8>(palCyan)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palGold)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palRose)),
                                           renderer.uploadPalette(std::span<const Rgba8>(palLime))};

    // Bold diagonal color bands → the rear scene reads as a vivid, distinct layer through any hole. Static.
    // Each cell names the rear sheet directly; the diagonal-band index picks one of the four bright palettes.
    std::vector<TileCell> rearCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int ty = 0; ty < kMapH; ++ty) {
        for (int tx = 0; tx < kMapW; ++tx) {
            const std::size_t band = ((static_cast<std::size_t>(tx) + ty) / 2) % 4;  // diagonal bands
            rearCells[static_cast<std::size_t>(ty) * kMapW + tx] =
                TileCell{.atlas = rearAtlas, .tile = 0, .palette = rearSet[band]};
        }
    }

    const std::array<Rgba8, 3> wallPal{{{0, 0, 0}, {176, 84, 64}, {224, 212, 196}}};  // brick + light mortar
    const PaletteId wallPalId = renderer.uploadPalette(std::span<const Rgba8>(wallPal));
    // Running bond: even rows draw tile 0 (seam at column 0), odd rows tile 1 (seam offset to column 4). Each
    // cell names the wall sheet + its one palette directly.
    std::vector<TileCell> wallCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int ty = 0; ty < kMapH; ++ty)
        for (int tx = 0; tx < kMapW; ++tx)
            wallCells[static_cast<std::size_t>(ty) * kMapW + tx] =
                TileCell{.atlas = wallAtlas, .tile = static_cast<std::uint16_t>(ty & 1), .palette = wallPalId};

    // Stencil controls.
    StencilMode mode      = StencilMode::TransparentInside;  // A toggles which side is see-through
    int         shapeIdx  = 0;                          // B cycles
    bool        soft      = false;                      // Up toggles feather hard ↔ soft
    bool        below     = false;                      // Down toggles Layer (reveal rear) ↔ Below (reveal backdrop)
    bool        effectsOn = true;                       // Start toggles the inside/outside region effects on/off
    bool        swapSides = false;                       // Right (East d-pad) swaps the inside/outside effects
    constexpr float kSoftFeather = 16.0f;

    int tick = 0;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::ToggleSide)) {
            mode = mode == StencilMode::TransparentInside ? StencilMode::TransparentOutside : StencilMode::TransparentInside;
            std::printf("[dev] %s\n", mode == StencilMode::TransparentInside ? "inside is see-through"
                                                                       : "outside is see-through");
        }
        if (in.justPressed(Action::CycleShape)) {
            ++shapeIdx;
            std::printf("[dev] shape: %s\n", shapeName(shapeIdx));
        }
        if (in.justPressed(Action::ToggleFeather)) {
            soft = !soft;
            std::printf("[dev] edge: %s\n", soft ? "feathered (soft)" : "hard");
        }
        if (in.justPressed(Action::ToggleScope)) {
            below = !below;
            std::printf("[dev] scope: %s\n", below ? "Below (see-through reveals the BACKDROP)"
                                                   : "Layer (see-through reveals the REAR scene)");
        }
        if (in.justPressed(Action::ToggleEffects)) {
            effectsOn = !effectsOn;
            std::printf("[dev] region effects: %s\n", effectsOn ? "ON (ripple inside, wave outside)"
                                                                : "OFF (plain see-through)");
        }
        if (in.justPressed(Action::SwapEffects)) {
            swapSides = !swapSides;
            std::printf("[dev] effects: %s\n", swapSides ? "swapped (wave inside, ripple outside)"
                                                         : "ripple inside, wave outside");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        ++tick;
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        // The region drifts slowly side to side (~8 s sweep) via a transform translation — one offset moves
        // every shape, the curve included. Gentle, no strobing.
        const float t  = static_cast<float>(tick) * 0.012f;
        const float dx = 34.0f * std::sin(t);

        ShapePoints region = shapeForIndex(shapeIdx);
        region.transform   = Transform::translation(dx, 0.0f);

        frame.layers.clear();
        DrawLayer rear{.key = "rearScene"};
        rear.z       = -10;
        rear.size    = PixelSize{kViewW, kViewH};
        rear.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                   .cells = std::span<const TileCell>(rearCells)};
        frame.layers.push_back(rear);

        DrawLayer wall{.key = "wall"};
        wall.z       = 0;
        wall.size    = PixelSize{kViewW, kViewH};
        wall.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                   .cells = std::span<const TileCell>(wallCells)};
        // The stencil() helper makes the wall SEE-THROUGH along `region` (nothing is erased — the pixels go
        // transparent so what's behind shows through). Layer scope reveals the rear scene; Below scope reveals
        // the backdrop. Both sides stay live regions: a gentle ripple plays in the INSIDE region and a slow
        // wave in the OUTSIDE region, so a see-through area is shown to carry an effect like a solid one.
        std::vector<ScreenSpaceEffect> insideEffects, outsideEffects;
        if (effectsOn) {  // Start toggles these — OFF leaves a plain see-through (the baseline)
            const ScreenSpaceEffect ripple{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 3.0f,
                                           .frequency = 6.0f, .phase = t,
                                           .center = {80.0f + dx, 72.0f}, .decay = 2.5f};
            const ScreenSpaceEffect wave{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f,
                                         .frequency = 2.0f, .phase = t * 0.5f, .axis = Axis::Horizontal};
            // Right (East d-pad) swaps which effect plays on which side — same two regions, contents flipped.
            insideEffects  = {swapSides ? wave : ripple};
            outsideEffects = {swapSides ? ripple : wave};
        }
        wall.regions = stencil(region, mode, soft ? kSoftFeather : 0.0f,
                               below ? ScreenSpaceEffectScope::Below : ScreenSpaceEffectScope::Layer,
                               insideEffects, outsideEffects);
        frame.layers.push_back(std::move(wall));

        renderer.renderFrame(frame);
    });

    std::printf("stencil demo — a Stencil makes the brick WALL SEE-THROUGH along a shape to reveal what's "
                "behind it (nothing is erased), with a ripple INSIDE the shape and a wave OUTSIDE it. "
                "A: inside / outside see-through. B: cycle shape. Up: hard / feathered edge. "
                "Down: Layer (reveal rear scene) / Below (reveal backdrop). Start: region effects on/off. "
                "Right: swap inside/outside effects. Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
