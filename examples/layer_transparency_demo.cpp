// Layer transparency demo — a runnable host showcasing the INDEXED-tile/palette compositor + opt-in
// per-source index-hole transparency. Open a window, LOAD A REAL COMMITTED PNG
// (examples/assets/demo_tiles.png) via loadPng, upload its index plane TWICE through the existing
// uploadAtlas — once OPAQUE (no transparent index) and once with transparent index 0 — color each
// through a hand-built palette set, and composite two role-free tile layers: a fully-opaque lower
// background (z=0) and, above it (z=10), the SAME art with index 0 declared transparent so its
// diamond-shaped index-0 regions become HOLES that reveal the opaque layer beneath. Then blit it
// integer-scaled + letterboxed onto the swapchain at display refresh, routing keyboard + gamepad
// input to the tick callback.
//
// Run it on a dev machine and confirm: the window shows two scrolling diamond fields in REAL
// COLOR; the LOWER field is fully opaque (its central diamonds are a solid palette color — the
// "no transparent index → faithful opaque background" control); the UPPER field's central diamonds
// are PUNCHED THROUGH (holes), revealing the lower field sliding past underneath as the two layers
// scroll at different rates — the same PNG, one upload opaque, one with an opt-in transparent index.
// Resizing re-letterboxes it, the close button quits, and pressing a mapped button prints a line.
//
// This is one of the runnable example hosts that instantiates SdlPlatform + Renderer in a real run,
// so it keeps the live SDL_GPU pipeline/upload/present path + the image-load front end compiling and
// linking on every CI platform even though CI never opens the window. (The beach demo —
// examples/beach_demo.cpp — is the companion that exercises the per-layer screen-space-effect path.)

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;

// The demo's input vocabulary: four directions whose edges print to the console, plus the four dev
// toggles the tick handles.
enum class Action : std::uint8_t {
    Up, Down, Left, Right,  // press/release edges print to the console
    ScaleCycle,             // X key / pad south — cycle the window scale 1×…8×
    WaveCycle,              // Z key / pad east — cycle the row-displacement wave mode
    SamplingToggle,         // Return / pad Start — blit sampling nearest ↔ bilinear
    Fullscreen,             // Backspace / pad Select — toggle native fullscreen
};

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

}  // namespace

int main() {

    // One startup config bundles window + viewport + timing; defaults are the faithful Game Boy
    // Color baseline — only the window title is overridden here.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Layer Transparency Demo"},
        .window = {.title = "Polyrhythm — layer transparency demo (index-hole)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Bind the demo's actions: the four directions on arrows + WASD + d-pad, and each dev toggle on
    // a key + a pad button.
    ActionMap map{
        {Action::ScaleCycle,     {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::WaveCycle,      {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::SamplingToggle, {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
    platform.actions(map);

    // Apply the startup presentation enhancements. The window already opened at
    // config.enhancements.windowScale (4×, clamped to the display) in the platform ctor; here we set
    // the blit sampler. windowScale is toggled live below; the renderer always auto-fills the window.
    int windowScale = config.enhancements.windowScale;  // live-toggled target (clamped on apply)
    int waveMode = 0;  // frame-level row-displacement: 0 off, 1 blank edge, 2 stretch edge

    // Load the real committed PNG (an engine-authored, license-clean indexed tileset: a 2×2-tile
    // atlas whose four tiles assemble a diamond centered on index 0 — the hole). loadPng extracts its
    // index plane; the embedded palette is ignored here (the demo hand-builds color).
    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }

    // Upload the SAME index plane twice: once fully opaque (transparent index −1, the default), once
    // with index 0 declared transparent so it renders as a hole. This is the headline: identical art,
    // one solid, one punched through — the per-source indexed transparency policy.
    const AtlasId opaqueAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height).atlasId;          // −1 = opaque
    const AtlasId holeAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height, TransparentIndices::of({0})).atlasId;

    // Two hand-built 4-entry palettes (index 0 = the would-be hole color on the opaque layer). Warm
    // for the opaque lower field, cool for the holed upper field — so revealing one through the other
    // is unmistakable.
    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));

    // A tilemap that lays the 2×2-tile atlas in repeating 2×2 super-blocks, so the diamond
    // reconstructs and repeats across the viewport. Kept alive for the program's duration. Each cell
    // now names its own sheet + palette directly, so the opaque lower field and the holed upper field
    // each get their own cell array (same tile layout, different atlas + palette).
    std::vector<TileCell> warmCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> coolCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kMapW + x;
            const auto t = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));  // 0,1 / 2,3 block
            warmCells[i] = TileCell{.atlas = opaqueAtlas, .tile = t, .palette = warmPal};
            coolCells[i] = TileCell{.atlas = holeAtlas, .tile = t,   .palette = coolPal};
        }
    }

    // The labeled actions the demo prints edges for.
    constexpr auto kLabels = std::to_array<std::pair<Action, const char*>>({
        {Action::Up, "Up"}, {Action::Down, "Down"}, {Action::Left, "Left"},
        {Action::Right, "Right"}, {Action::ScaleCycle, "ScaleCycle"},
        {Action::WaveCycle, "WaveCycle"}, {Action::SamplingToggle, "SamplingToggle"},
        {Action::Fullscreen, "Fullscreen"},
    });

    auto familyName = [](ControllerType t) {
        switch (t) {
            case ControllerType::Xbox:        return "Xbox";
            case ControllerType::PlayStation: return "PlayStation";
            case ControllerType::Nintendo:    return "Nintendo";
            case ControllerType::Standard:    return "Standard";
            default:                          return "none";
        }
    };

    ActiveDevice lastDevice{};
    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        const ActiveDevice device = in.activeDevice();  // the per-slot active-device signal
        if (device != lastDevice) {
            lastDevice = device;
            if (device.kind == DeviceKind::Gamepad) {
                std::printf("controller: %s\n", familyName(device.family));
            }
        }
        for (const auto& [action, name] : kLabels) {
            if (in.justPressed(action))  std::printf("press   %s\n", name);
            if (in.justReleased(action)) std::printf("release %s\n", name);
        }

        // Live verification — the DEV toggles (demo only):
        //   Fullscreen     → toggle native fullscreen (a real macOS Space) and back
        //   SamplingToggle → toggle blit sampling nearest ↔ bilinear (crisp ↔ smoothed)
        //   ScaleCycle     → cycle the window scale 1×…8× — resize the window to that multiple of
        //                    the viewport (clamped to the display), the content auto-fills it crisply
        if (in.justPressed(Action::Fullscreen)) {
            platform.window().fullscreen(!platform.window().fullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.window().fullscreen() ? "on" : "off");
        }
        if (in.justPressed(Action::SamplingToggle)) {
            const bool bilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(bilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", bilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::WaveCycle)) {
            waveMode = (waveMode + 1) % 3;  // off → blank edge → stretch edge → off
            const char* names[] = {"off", "on (blank edge)", "on (stretch edge)"};
            std::printf("[dev] row-displacement: %s\n", names[waveMode]);
        }
        if (in.justPressed(Action::ScaleCycle)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;  // 1→2→…→8→1
            const PixelSize vp{config.viewport.width, config.viewport.height};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.window().fullscreen()) {
                platform.window().size(PixelSize{vp.width * eff, vp.height * eff});
            }
            if (eff != windowScale) {
                std::printf("[dev] window scale: %d× requested, clamped to %d× (display limit)\n",
                            windowScale, eff);
            } else {
                std::printf("[dev] window scale: %d×\n", eff);
            }
        }
    });

    // The game owns the draw state; the render callback rebuilds + scrolls it each advance(). It
    // stacks TWO role-free tile layers from the SAME PNG: the opaque lower field, and above it the
    // holed upper field whose index-0 diamonds reveal the lower field through the holes.
    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();

        const int drift = tick / 6;  // ~10 px/s

        // z=0: the fully-opaque lower field (no transparent index → faithful opaque), drifting slowly.
        DrawLayer lower{.key = "opaqueLowerField"};
        lower.z       = 0;
        lower.size    = PixelSize{160, 144};
        lower.scroll  = LayerScroll{drift / 2, 0};
        lower.alpha   = 1.0f;
        lower.content = TileContent{.widthInTiles  = kMapW,
                                    .heightInTiles = kMapH,
                                    .cells         = std::span<const TileCell>(warmCells)};
        frame.layers.push_back(std::move(lower));

        // z=10: the same art with index 0 transparent — its diamonds are HOLES revealing the lower
        // field beneath. Drifts the SAME direction a touch faster, so the reveal slides calmly.
        DrawLayer upper{.key = "holedUpperField"};
        upper.z       = 10;
        upper.size    = PixelSize{160, 144};
        upper.scroll  = LayerScroll{drift, drift / 4};
        upper.alpha   = 1.0f;
        upper.content = TileContent{.widthInTiles  = kMapW,
                                    .heightInTiles = kMapH,
                                    .cells         = std::span<const TileCell>(coolCells)};
        frame.layers.push_back(std::move(upper));

        // A frame-level row-displacement post-process, cycled by B (off → blank edge → stretch edge).
        // Empty postEffects (waveMode == 0) leaves the output untouched.
        frame.postEffects.clear();
        if (waveMode != 0) {
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement,
                .amplitude = 4.0f,                            // ±4 viewport px
                .frequency = 3.0f,                            // 3 wave crests down the screen
                .phase     = static_cast<float>(tick) * 0.01f,  // ~0.6 cycles/s — calm drift
                .axis      = Axis::Horizontal,
                .edge      = (waveMode == 2) ? DisplacementEdge::Stretch : DisplacementEdge::Blank});
        }

        // No frame-level modifier/blend (identity) — the composited output is unchanged.
        renderer.renderFrame(frame);

    });

    std::printf("layer transparency demo — a real indexed PNG uploaded twice (opaque lower field + a "
                "holed upper field whose index-0 diamonds reveal the lower field through the holes); "
                "close to quit.\n");
    std::printf("[dev] Backspace / pad Select = fullscreen, Return / pad Start = nearest/bilinear, "
                "X / pad south = cycle window scale (1×–8×, clamped to display), Z / pad east = "
                "frame-level row-displacement wave.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
