// ColorFill demo — a runnable host that VISUALLY proves the built-in region-confinable colour fill: a
// colour painted onto a Region's shape. It opens a window over an opaque dim-grid backdrop and draws four
// things with the SAME built-in effect (ScreenSpaceEffectKind::ColorFill), each confined to a Region:
//
//   • a SOLID filled rectangle — a solid coloured shape;
//   • a STROKED ring (a circle + strokeWidth) — a coloured outline / hoop;
//   • a CURVED drawn LINE (ShapePoints::fromCurve on an OPEN curve + strokeWidth) — the missing primitive:
//     a stroked region filled with a colour IS a drawn colored path;
//   • a TRANSLUCENT warm tint — a solid colour in a Region whose alpha (0.5) blends it over the backdrop.
//
// All four are frame-level regions (FrameDrawState::regions). A fill emits its colour into the region's
// shape, so it paints the same way wherever it is attached; the Region's alpha and blend mode are what
// compose it over the backdrop, which is how the tint gets its translucency. The
// pixel-exact colour math is the device-free ctest suite's job (applyColorFill vs the shader); this is
// the live GPU sanity check.
//
// The window never auto-launches (a dev drives it). Backspace = fullscreen; close to quit.

#include <array>
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

// The demo's input vocabulary: one dev key.
enum class Action : std::uint8_t { Fullscreen };

// A solid colour fill (paint the region's shape this colour, ignoring what was under it). The colour is
// opaque (an Rgba8's alpha defaults to 255), so it covers whatever was under the shape.
[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Color Fill Demo"},
        .window = {.title = "Polyrhythm — colour-fill demo (solid / stroke / drawn line / tint)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // An opaque dim-grid backdrop so the fills/lines have something to paint onto and the tint has texture
    // to grade. A faint two-tone 8×8 grid tile (index 1 fill, index 2 on the top/left edge).
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;

    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {40, 44, 62}, {58, 64, 90}}};  // opaque dim slate grid
    const PaletteId gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell>    gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // The four shapes (viewport pixels):
    // 1 — a solid filled rectangle (top-left).
    const ShapePoints solidRect = ShapePoints::rectangle(Point{12, 16}, 56, 38);
    // 2 — a stroked ring (top-right): a circle whose effects confine to a band along its boundary.
    ShapePoints       ring = ShapePoints::circle(Point{122, 38}, 22);
    ring.strokeWidth       = 6.0f;
    // 3 — a CURVED drawn LINE (bottom): an OPEN two-segment quadratic curve, stroked into an open band — a
    // stroked region filled with colour is a drawn colored path (the headline: real vector lines).
    Curve wavy;
    wavy.closed   = false;
    wavy.segments = {CurveSegment{Vec2{18, 116}, Vec2{46, 98}, Vec2{74, 116}, Vec2{}, CurveDegree::Quadratic},
                     CurveSegment{Vec2{74, 116}, Vec2{102, 134}, Vec2{142, 112}, Vec2{}, CurveDegree::Quadratic}};
    ShapePoints drawnLine = ShapePoints::fromCurve(wavy);
    drawnLine.strokeWidth = 5.0f;
    // 4 — a TRANSLUCENT tint (right side): a solid colour in a Region whose alpha makes it see-through.
    const ShapePoints tintRect = ShapePoints::rectangle(Point{96, 64}, 52, 30);

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        frame.regions.clear();
        frame.regions.push_back(Region{.key = "cyan",    .shape = solidRect, .effects = {solidFill(Rgba8{31, 219, 255})}});  // cyan solid
        frame.regions.push_back(Region{.key = "magenta", .shape = ring,      .effects = {solidFill(Rgba8{255, 56, 224})}});  // magenta ring
        frame.regions.push_back(Region{.key = "yellow",  .shape = drawnLine, .effects = {solidFill(Rgba8{255, 214, 26})}});  // yellow drawn line
        frame.regions.push_back(Region{.key = "tint",    .shape = tintRect, .effects = {solidFill(Rgba8{200, 90, 0})}, .alpha = 0.5f});  // translucent warm tint

        renderer.renderFrame(frame);
    });

    std::printf("colour-fill demo — a SOLID cyan rectangle, a STROKED magenta ring, a CURVED yellow drawn "
                "LINE (fromCurve + stroke = a real vector path), and a TRANSLUCENT warm TINT (a Region with "
                "alpha). All one built-in: ScreenSpaceEffectKind::ColorFill confined by a Region. Static "
                "scene; Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
