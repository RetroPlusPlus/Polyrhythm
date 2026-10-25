// Tween demo — a runnable host proving the value-animation thesis across THREE kinds of draw-state sink
//: a layer field, a frame-wide color transform, and a SHADER/EFFECT parameter. Open a
// window, load a real committed PNG (examples/assets/demo_tiles.png), and composite two role-free tile
// layers in real color: a fully-opaque lower background and, above it, the same art in a different
// palette. Three game-owned TweenPlayers drive existing draw-state values, each writing value() into the
// draw state every frame; the engine never touches a tween (immediate-mode lock preserved):
//
//   1. LAYER ALPHA (scalar)   — TweenPlayer<float> yoyos the upper field's alpha 1→0→1, dissolving it to
//                               reveal the lower field, then restoring it.
//   2. FRAME color grade (vector) — TweenPlayer<Vec3> ramps a Multiply ColorFill region from noon (white) toward
//                               a dim dusk tint and back — the day/night case.
//   3. SHADER / EFFECT PARAMETER (scalar) — TweenPlayer<float> swells the built-in Ripple effect's
//                               AMPLITUDE (a value that becomes a shader uniform) 0→6px→0, so a radial
//                               water ripple grows and recedes over the composited frame. Effect
//                               parameters are not special — they are one more draw-state value a tween
//                               animates. (To animate a CUSTOM shader's own param, the call is identical:
//                               set ScreenSpaceEffect{ .kind = Custom, .customShader = h, .<param> = … }
//                               and write a TweenPlayer's value() into <param> the same way.)
//
// All three are authored as 2-segment yoyo tracks under LoopIndefinitely. Run it on a dev machine and
// confirm: the upper diamond field slowly fades to reveal the lower field and back; the whole scene
// drifts toward dusk and back; a radial ripple swells from the center and recedes. The window does
// NOT auto-launch.
//
// X (pad south) pauses/resumes the dusk ramp (pause/play the color player); Z (pad east) restarts all
// players. This is one of the runnable example hosts that instantiates SdlPlatform + Renderer, so it
// keeps the live path compiling on every CI platform even though CI never opens the window.

#include <array>
#include <chrono>
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
#include "retropp/tween.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;
using namespace std::chrono_literals;

constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;

// The demo's input vocabulary: pause/resume the dusk ramp, and restart every player.
enum class Action : std::uint8_t { DuskToggle, RestartAll };

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Tween Demo"},
        .window = {.title = "Polyrhythm — tween demo (layer alpha + dusk colour grade)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::DuskToggle, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::RestartAll, {SDL_SCANCODE_Z, PadButton::FaceEast}},
    };
    platform.actions(map);

    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }
    const AtlasId atlas = renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height).atlasId;

    // Warm lower field, cool upper field — so the upper fading away to reveal the lower is unmistakable.
    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));

    // Each cell names its own sheet + palette directly. Both layers draw the same diamond map from the same
    // sheet, but in different palettes (warm lower / cool upper), so each layer gets its own cell array.
    std::vector<TileCell> warmCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> coolCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const auto tile = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));
            const std::size_t i = static_cast<std::size_t>(y) * kMapW + x;
            warmCells[i] = TileCell{.atlas = atlas, .tile = tile, .palette = warmPal};
            coolCells[i] = TileCell{.atlas = atlas, .tile = tile, .palette = coolPal};
        }
    }

    // ── The tweens (game-owned; outlive the players below) ──────────────────────────────────────────
    // Layer alpha: a slow fade-out-and-back yoyo — opaque (1) → transparent (0) over 3s, then back over
    // 3s, forever. InOutSine so the dissolve eases in and out (no hard edges).
    const Tween<float> alphaTween =
        Tween<float>::of(1.0f, 0.0f, 3s, Easing::InOutSine).then(1.0f, 3s, Easing::InOutSine);
    // Frame color-grade multiplier: noon (1,1,1) → a dim dusk tint and back over 5s each way. The Vec3
    // is the per-channel multiply; the demo writes it into a Multiply ColorFill region's fill.
    const Tween<Vec3> duskTween =
        Tween<Vec3>::of(Vec3{1, 1, 1}, Vec3{0.45f, 0.35f, 0.55f}, 5s, Easing::InOutSine)
            .then(Vec3{1, 1, 1}, 5s, Easing::InOutSine);
    // Effect/SHADER parameter: the built-in Ripple's amplitude (a value packed into the ripple shader's
    // cbuffer) swelling 0 → 6px → 0 over 4s each way. Tweening this animates a shader uniform.
    const Tween<float> rippleAmpTween =
        Tween<float>::of(0.0f, 6.0f, 4s, Easing::InOutSine).then(0.0f, 4s, Easing::InOutSine);

    TweenPlayer<float> alphaPlayer{.tween = &alphaTween};
    TweenPlayer<Vec3>  duskPlayer{.tween = &duskTween};
    TweenPlayer<float> ripplePlayer{.tween = &rippleAmpTween};

    constexpr auto kLabels = std::to_array<std::pair<Action, const char*>>({
        {Action::DuskToggle, "DuskToggle"}, {Action::RestartAll, "RestartAll"},
    });

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        for (const auto& [action, name] : kLabels) {
            if (in.justPressed(action)) std::printf("press %s\n", name);
        }
        // DuskToggle → freeze/resume the dusk ramp (showcases pause/play on a value player);
        // RestartAll → restart every player.
        if (in.justPressed(Action::DuskToggle)) {
            duskPlayer.playing ? duskPlayer.pause() : duskPlayer.play();
            std::printf("[dev] dusk ramp: %s\n", duskPlayer.playing ? "playing" : "paused");
        }
        if (in.justPressed(Action::RestartAll)) {
            alphaPlayer.restart();
            duskPlayer.restart();
            ripplePlayer.restart();
            std::printf("[dev] all players restarted\n");
        }

        // Advance the value players on the sim tick under LoopIndefinitely (the yoyo loops forever).
        alphaPlayer.advance(PlaybackMode::loopIndefinitely());
        duskPlayer.advance(PlaybackMode::loopIndefinitely());
        ripplePlayer.advance(PlaybackMode::loopIndefinitely());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        const int drift = tick / 6;  // ~10 px/s gentle same-direction parallax

        DrawLayer lower{.key = "warmLowerField"};
        lower.z       = 0;
        lower.size    = PixelSize{160, 144};
        lower.scroll  = LayerScroll{drift / 2, 0};
        lower.alpha   = 1.0f;
        lower.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(warmCells)};
        frame.layers.push_back(std::move(lower));

        DrawLayer upper{.key = "coolUpperField"};
        upper.z       = 10;
        upper.size    = PixelSize{160, 144};
        upper.scroll  = LayerScroll{drift, drift / 4};
        upper.alpha   = alphaPlayer.value();  // ← THE TWEEN: layer alpha sink (scalar)
        upper.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(coolCells)};
        frame.layers.push_back(std::move(upper));

        // ← THE TWEEN: a whole-frame color grade as an ordinary effect (vector, component-wise). A
        // Multiply-blended ColorFill region covering the viewport; its fill is the per-channel multiplier —
        // white (1,1,1) at noon multiplies to no change, a dim tint at dusk multiplies the whole frame down.
        // The tween drives the fill color. Whole-frame color is just an effect; there is no frame member.
        const Vec3 mul = duskPlayer.value();
        const auto u8  = [](float v) { return static_cast<std::uint8_t>(v * 255.0f); };
        frame.regions.clear();
        frame.regions.push_back(Region{
            .key     = "dusk",
            .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                          .fill = Rgba8{u8(mul.x), u8(mul.y), u8(mul.z), 255}}},
            .blend   = BlendMode::Multiply});

        // ← THE TWEEN: a SHADER/EFFECT parameter. A frame-level built-in Ripple whose AMPLITUDE (a ripple
        // shader uniform) is the tweened value — the ripple swells from the center and recedes. The rest
        // of the effect's params are static; `phase` advances monotonically off the tick so the rings
        // drift outward calmly. Amplitude 0 (the yoyo's resting points) → the frame is the faithful
        // baseline. This is the headline: an effect parameter is just another draw-state value a tween
        // drives — a custom shader's own reflected param would be written here the exact same way.
        frame.postEffects.clear();
        frame.postEffects.push_back(ScreenSpaceEffect{
            .kind      = ScreenSpaceEffectKind::Ripple,
            .amplitude = ripplePlayer.value(),               // ← the tweened shader parameter
            .frequency = 5.0f,                                // 5 rings across the field
            .phase     = static_cast<float>(tick) * 0.01f,    // ~0.6 cycles/s — rings drift out slowly
            .center    = Point{80, 72},                       // viewport center (160×144)
            .decay     = 1.5f});

        renderer.renderFrame(frame);
    });

    std::printf("tween demo — the upper field fades to reveal the lower one and back (layer-alpha tween); "
                "the frame ramps toward dusk and back (a Multiply ColorFill region the tween drives); a radial ripple swells and "
                "recedes (the ripple EFFECT's amplitude tween — a shader parameter). Close to quit.\n");
    std::printf("[dev] X (pad south) = pause/resume the dusk ramp, Z (pad east) = restart all players.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
