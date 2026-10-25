// Region-confined effects, focused example #6 — a built-in effect confined to a region.
//
// One idea: the region gate is engine-side, so it confines ANY screen-space effect to a shape with no
// change to the effect itself. This uses the engine's BUILT-IN radial ripple (ScreenSpaceEffectKind::
// Ripple, promoted from a consumer custom shader) and confines it to a circle: the droplet
// rings expand only inside the porthole, the rest of the grid is still. The ToggleGate action (Z key /
// pad east) switches the region on/off so you can see the same effect run whole-frame vs gated.
//
// Opens a real window so the live effect + gate path keep compiling on every CI platform.

#include <array>
#include <cstddef>
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

// The demo's input vocabulary: the gate toggle plus one dev toggle.
enum class Action : std::uint8_t { ToggleGate, Fullscreen };
}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Region Ripple Demo"},
        .window = {.title = "Polyrhythm — custom shader in a region"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the demo's actions: the gate toggle on Z or the pad's east face button, fullscreen on
    // Backspace or the pad's Select.
    ActionMap map{
        {Action::ToggleGate, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    std::array<std::uint8_t, 64> grid{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            grid[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId atlas = renderer.uploadAtlas(grid.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> pal{{{0, 0, 0}, {44, 70, 120}, {180, 210, 255}}};
    const PaletteId p = renderer.uploadPalette(std::span<const Rgba8>(pal));
    // Each cell names its sheet (`atlas`) and palette directly — there is no per-layer set.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                TileCell{.atlas = atlas, .tile = 0, .palette = p});

    bool gated = true;  // ToggleGate: confine the ripple to a circle vs run it whole-frame
    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::ToggleGate)) { gated = !gated; std::printf("[dev] ripple region: %s\n", gated ? "circle" : "whole frame"); }
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
        frame.layers.push_back(bg);

        // The BUILT-IN ripple, centered in viewport pixels (the engine normalizes to UV).
        ScreenSpaceEffect rip{
            .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f, .frequency = 6.0f,
            .phase = static_cast<float>(tick) * 0.012f,
            .center = {kViewW / 2.0f, kViewH / 2.0f}, .decay = 2.5f};
        frame.postEffects.clear();
        frame.regions.clear();
        if (gated)  // SAME effect, now confined to a region the frame owns
            frame.regions.push_back(Region{.key = "ripple", .shape = ShapePoints::circle({80, 72}, 40), .effects = {rip}});
        else
            frame.postEffects.push_back(rip);

        renderer.renderFrame(frame);
    });

    std::printf("Built-in ripple in a region — the ripple confined to a circle (engine-side gate, "
                "effect untouched). Z / pad east toggles circle vs whole frame. Backspace / pad Select = "
                "fullscreen.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
