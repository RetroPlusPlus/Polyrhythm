// Region-confined effects, focused example #3 — MOVING a shaped effect (the smallest example).
//
// One idea: because the whole frame's draw state is recomputed each frame, you move a shaped effect
// just by giving its region new coordinates every frame — no animation API. A circular wavy "porthole"
// glides slowly left↔right across a static grid: inside the moving circle the grid waves, everywhere
// else it is still. The circle's center is recomputed each frame from a slow sine.
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
#include "retropp/windowed_host.h"

namespace {
using namespace retropp;
constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

// The demo's input vocabulary: one dev toggle.
enum class Action : std::uint8_t { Fullscreen };
}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Region Motion Demo"},
        .window = {.title = "Polyrhythm — moving region"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the demo's one action: fullscreen on Backspace or the pad's Select.
    ActionMap map{
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {96, 72, 132}, {214, 196, 248}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));
    // Each cell names its sheet (`atlas`) and palette directly — there is no per-layer set.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                TileCell{.atlas = atlas, .tile = 0, .palette = p});

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "grid"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(cells)};

        // The ONLY moving part: the circle's center, recomputed each frame. Slow horizontal glide.
        const float cx = 80.0f + 56.0f * std::sin(static_cast<float>(tick) * 0.01f);
        bg.regions = {Region{
            .key     = "porthole",
            .shape   = ShapePoints::circle({cx, 72.0f}, 30.0f),
            .effects = {ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f, .frequency = 3.0f,
                .phase = static_cast<float>(tick) * 0.006f, .axis = Axis::Horizontal,
                .scope = ScreenSpaceEffectScope::Layer}}}};
        frame.layers.push_back(bg);

        renderer.renderFrame(frame);
    });

    std::printf("Moving region — a wavy circular porthole glides across a still grid; move it by "
                "just recomputing the region's centre each frame. Backspace / pad Select = fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
