// Region-confined effects, focused example #1 — the SHAPE VOCABULARY.
//
// One idea: an effect's `region` (ShapePoints) confines it to a shape. A single horizontal
// RowDisplacement wave runs over a scrolling grid, but ONLY inside the current region — outside the
// shape the grid is undisturbed. Press Z (or the pad's east button) to cycle the shape through every
// preset:
//   circle → capsule → triangle → rectangle → roundedRectangle → regularPolygon(hexagon) → none
// `none` (count 0) removes the region so the wave covers the whole viewport — the byte-identical
// pre-region baseline. The points are VIEWPORT PIXELS; the shape just sits where its coordinates put
// it. Watch the wavy patch take each shape in turn.
//
// Like the other example hosts this opens a real window with SdlPlatform + Renderer, so the live gate
// path keeps compiling + linking on every CI platform even though CI never opens the window.

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
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

namespace {
using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

// The demo's input vocabulary: the shape cycler plus one dev toggle.
enum class Action : std::uint8_t { NextShape, Fullscreen };

// The current region for a cycle index (NextShape steps it). The points are viewport pixels; everything is
// placed around the screen center (80, 72) so each shape sits over the same patch.
ShapePoints shapeForIndex(int i) {
    switch (i % 7) {
        case 0:  return ShapePoints::circle({80, 72}, 34);
        case 1:  return ShapePoints::capsule({44, 72}, {116, 72}, 22);
        case 2:  return ShapePoints::triangle({80, 34}, {126, 110}, {34, 110});
        case 3:  return ShapePoints::rectangle({40, 44}, 80, 56);
        case 4:  return ShapePoints::roundedRectangle({36, 40}, 88, 64, 18);
        case 5:  return ShapePoints::regularPolygon({80, 72}, 40, 6);  // hexagon
        default: return ShapePoints{};                                  // none → whole viewport
    }
}
const char* shapeName(int i) {
    switch (i % 7) {
        case 0: return "circle";          case 1: return "capsule";
        case 2: return "triangle";        case 3: return "rectangle";
        case 4: return "roundedRectangle"; case 5: return "regularPolygon (hexagon)";
        default: return "none (whole viewport)";
    }
}
}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Region Shapes Demo"},
        .window = {.title = "Polyrhythm — region shapes"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the demo's actions: the shape cycler on Z or the pad's east face button, fullscreen on
    // Backspace or the pad's Select.
    ActionMap map{
        {Action::NextShape,  {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A grid tile (border index 2 over fill index 1) repeated everywhere — the displacement waves its
    // lines, so the effect's reach is obvious. Two palettes checkerboarded for legibility.
    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8).atlasId;

    const std::array<Rgba8, 3> palA{{{0, 0, 0}, {54, 78, 140}, {150, 188, 255}}};
    const std::array<Rgba8, 3> palB{{{0, 0, 0}, {140, 60, 96}, {255, 178, 210}}};
    const PaletteId pa = renderer.uploadPalette(std::span<const Rgba8>(palA));
    const PaletteId pb = renderer.uploadPalette(std::span<const Rgba8>(palB));
    const std::array<PaletteId, 2> palettes{pa, pb};  // checkerboarded by (x+y)&1

    // Each cell names its sheet (`atlas`) and palette directly; the checkerboard picks one of the two
    // palettes per cell.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            cells[static_cast<std::size_t>(y) * kMapW + x] =
                TileCell{.atlas = atlas, .tile = 0, .palette = palettes[(x + y) & 1]};

    int shapeIdx = 0;
    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::NextShape)) {
            ++shapeIdx;
            std::printf("[dev] region shape: %s\n", shapeName(shapeIdx));
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "grid"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.scroll  = LayerScroll{tick / 8, 0};  // slow same-direction drift
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(cells)};
        // The wave — confined to the current shape. Everything ELSE about the effect is ordinary; the
        // owning Region's shape makes it local. An empty shape (shapeIdx % 7 == 6) covers the whole viewport.
        bg.regions = {Region{
            .key     = "shape",
            .shape   = shapeForIndex(shapeIdx),
            .effects = {ScreenSpaceEffect{
                .kind      = ScreenSpaceEffectKind::RowDisplacement,
                .amplitude = 4.0f,
                .frequency = 2.5f,
                .phase     = static_cast<float>(tick) * 0.006f,
                .axis      = Axis::Horizontal,
                .scope     = ScreenSpaceEffectScope::Layer}}}};
        frame.layers.push_back(bg);

        renderer.renderFrame(frame);
    });

    std::printf("Region shapes — a horizontal wave confined to a shape. Z / pad east cycles "
                "circle/capsule/triangle/rectangle/roundedRectangle/hexagon/none. Backspace / pad "
                "Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
