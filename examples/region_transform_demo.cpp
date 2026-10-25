// Region-confined effects, focused example #2 — the region's TRANSFORM.
//
// One idea: a region carries a `Transform`, so the SHAPE confining an effect can be scaled,
// stretched (non-uniform scale), skewed, and rotated — exactly like a layer. A rectangle region holds a
// horizontal wave; press Z (or the pad's east button) to cycle how its transform animates:
//   scale pulse → stretch (non-uniform) → skew → rotate → identity
// The wave stays a plain horizontal wave; only the shape it lives in warps. The transform is composed on
// top of the points (which stay put), about the shape's center.
//
// Opens a real window so the live gate path keeps compiling on every CI platform.

#include <array>
#include <cmath>
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
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {
using namespace retropp;
constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;
constexpr float kCx = 80.0f, kCy = 72.0f;   // the region's center / transform pivot

// The demo's input vocabulary: the mode cycler plus one dev toggle.
enum class Action : std::uint8_t { NextMode, Fullscreen };

enum Mode { ScalePulse, Stretch, Skew, Rotate, Identity, kModeCount };
const char* modeName(int m) {
    switch (m) {
        case ScalePulse: return "scale pulse";  case Stretch: return "stretch (non-uniform scale)";
        case Skew:       return "skew";          case Rotate:  return "rotate";
        default:         return "identity";
    }
}
// The animated transform for a mode at tick t. Composed about the region center.
Transform transformFor(int mode, int t) {
    const float ft = static_cast<float>(t);
    switch (mode) {
        case ScalePulse: { const float s = 1.0f + 0.4f * std::sin(ft * 0.02f); return Transform::scale(s, s, kCx, kCy); }
        case Stretch:    { const float sx = 1.0f + 0.6f * std::sin(ft * 0.02f); return Transform::scale(sx, 0.7f, kCx, kCy); }
        case Skew:       { const float k = 0.6f * std::sin(ft * 0.02f);         return Transform::skew(k, 0.0f, kCx, kCy); }
        case Rotate:     return Transform::rotation(ft * 0.4f, kCx, kCy);  // slow spin
        default:         return Transform{};                               // identity
    }
}
}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Region Transform Demo"},
        .window = {.title = "Polyrhythm — region transform"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the demo's actions: the mode cycler on Z or the pad's east face button, fullscreen on
    // Backspace or the pad's Select.
    ActionMap map{
        {Action::NextMode,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {52, 110, 92}, {170, 240, 200}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));

    // Each cell names its sheet (`atlas`) and palette directly — there is no per-layer set.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                TileCell{.atlas = atlas, .tile = 0, .palette = p});

    int mode = ScalePulse;
    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::NextMode)) { mode = (mode + 1) % kModeCount; std::printf("[dev] transform: %s\n", modeName(mode)); }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "grid"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.scroll  = LayerScroll{tick / 10, 0};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(cells)};

        // A fixed rectangle region, warped by the animated transform. The points never change — the
        // transform does all the scale/stretch/skew/rotate.
        ShapePoints region = ShapePoints::rectangle({kCx - 44, kCy - 30}, 88, 60);
        region.transform   = transformFor(mode, tick);
        bg.regions = {Region{
            .key     = "warped",
            .shape   = region,
            .effects = {ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 2.5f,
                .phase = static_cast<float>(tick) * 0.006f, .axis = Axis::Horizontal,
                .scope = ScreenSpaceEffectScope::Layer}}}};
        frame.layers.push_back(bg);

        renderer.renderFrame(frame);
    });

    std::printf("Region transform — a rectangular wavy region scaled/stretched/skewed/rotated. "
                "Z / pad east cycles the mode. Backspace / pad Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
