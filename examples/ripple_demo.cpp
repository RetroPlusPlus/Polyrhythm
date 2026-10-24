// Ripple demo — a runnable host showcasing the engine's BUILT-IN radial ripple effect.
// Based on the layer-transparency demo (two scrolling indexed-PNG tile layers in real color); the
// addition is the built-in ripple, named and parameterized like any other screen-space effect.
//
// The ripple (ScreenSpaceEffectKind::Ripple) is a RADIAL water-droplet RIPPLE emanating from the center
// of the screen — concentric rings expanding slowly outward. It does something the axis-aligned built-in
// RowDisplacement cannot, and it is now a first-class member of the engine's effect library: the game
// names the kind (.kind = ScreenSpaceEffectKind::Ripple) and sets its fields — no shader, no registration.
// The demo also stacks the ripple WITH RowDisplacement to confirm two built-ins compose in one chain.
//
// (The ripple once lived as a consumer-authored custom shader here, demonstrating the
// custom-shader hook; it was promoted to a built-in, so this demo no longer registers a fragment. The
// custom-shader registration path + its device-free tests remain; the bespoke swirl restores the
// consumer-fragment-generates-on-all-backends CI signal.)
//
// Run it on a dev machine and confirm: the two diamond fields scroll as before; pressing Z drops a
// ripple in the center and rings expand outward across the whole frame; pressing Up adds the built-in
// horizontal wave on top, the two composing; both off restores the faithful frame.

#include <array>
#include <cstddef>
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

constexpr int kViewW = 160, kViewH = 144;  // the faithful GBC viewport
constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;

// The demo's input vocabulary: each dev toggle is its own action.
enum class Action : std::uint8_t { RippleToggle, WaveToggle, Fullscreen, SamplingToggle };

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Ripple Demo"},
        .window = {.title = "Polyrhythm — custom shader demo (radial ripple)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::RippleToggle,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::WaveToggle,     {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::SamplingToggle, {SDL_SCANCODE_RETURN, PadButton::Start}},
    };
    platform.actions(map);

    bool rippleOn = false;  // Z — the built-in radial ripple
    bool waveOn   = false;  // Up — the built-in RowDisplacement (to show the two compose)

    // Load the committed indexed PNG (a 2×2-tile diamond atlas centered on index 0).
    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }

    const AtlasId opaqueAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height).atlasId;
    const AtlasId holeAtlas =
        renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height, TransparentIndices::of({0})).atlasId;

    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));

    // Each cell names its own sheet + palette directly. The two layers draw the same diamond map but from
    // different sheets (opaque / holed) and palettes (warm / cool), so each gets its own cell array.
    std::vector<TileCell> lowerCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> upperCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const auto tile = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));
            const std::size_t i = static_cast<std::size_t>(y) * kMapW + x;
            lowerCells[i] = TileCell{.atlas = opaqueAtlas, .tile = tile, .palette = warmPal};
            upperCells[i] = TileCell{.atlas = holeAtlas, .tile = tile,   .palette = coolPal};
        }
    }

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        // Live verification — DEV toggle actions (demo only):
        if (in.justPressed(Action::Fullscreen)) {
            platform.window().fullscreen(!platform.window().fullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.window().fullscreen() ? "on" : "off");
        }
        if (in.justPressed(Action::SamplingToggle)) {
            const bool bilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(bilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", bilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::RippleToggle)) {
            rippleOn = !rippleOn;
            std::printf("[dev] custom ripple: %s\n", rippleOn ? "on" : "off");
        }
        if (in.justPressed(Action::WaveToggle)) {
            waveOn = !waveOn;
            std::printf("[dev] built-in row-displacement: %s\n", waveOn ? "on" : "off");
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        const int drift = tick / 6;  // gentle same-direction parallax (~10 px/s); no strobing moiré

        DrawLayer lower{.key = "opaqueLowerField"};
        lower.z       = 0;
        lower.size    = PixelSize{160, 144};
        lower.scroll  = LayerScroll{drift / 2, 0};
        lower.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(lowerCells)};
        frame.layers.push_back(std::move(lower));

        DrawLayer upper{.key = "holedUpperField"};
        upper.z       = 10;
        upper.size    = PixelSize{160, 144};
        upper.scroll  = LayerScroll{drift, drift / 4};
        upper.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(upperCells)};
        frame.layers.push_back(std::move(upper));

        // The post-process chain composes two BUILT-IN effects — the radial ripple and the axis-aligned
        // wave — in submission order: the headline is that the ripple is a first-class effect kind, used
        // exactly where RowDisplacement is. Empty chain (both off) leaves the output untouched.
        frame.postEffects.clear();
        if (rippleOn) {
            // center in VIEWPORT PIXELS — screen center (the droplet impact point); the engine normalizes
            // it to UV. ±6 px amplitude, ~6 rings, slow outward phase, radial decay (droplet falloff).
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::Ripple, .amplitude = 6.0f, .frequency = 6.0f,
                .phase = static_cast<float>(tick) * 0.012f,
                .center = {kViewW / 2.0f, kViewH / 2.0f}, .decay = 2.5f});
        }
        if (waveOn) {
            frame.postEffects.push_back(ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f, .frequency = 3.0f,
                .phase = static_cast<float>(tick) * 0.01f, .axis = Axis::Horizontal});
        }

        renderer.renderFrame(frame);
    });

    std::printf("ripple demo — the engine's BUILT-IN radial ripple; rings expand from the centre like a "
                "dropped water droplet. Close to quit.\n");
    std::printf("[dev] Z (pad east) = built-in ripple, Up = built-in row-displacement (stack them — they "
                "compose), Backspace (pad Select) = fullscreen, Return (pad Start) = nearest/bilinear.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
