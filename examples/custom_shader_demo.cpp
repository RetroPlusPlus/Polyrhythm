// Custom shader-stage demo — the game-registered custom shader hook, showing the
// hook's actual purpose: effects too WEIRD and USELESS to ever be engine built-ins. Built-in effects
// (ripple, the wave) have a use case and live in the engine; the custom path is the long tail. So this
// demo registers THREE deliberately-pointless consumer shaders and lets you switch between them:
//
//   1. Mirror-ghost   — a faint, point-mirrored copy of the frame cross-fades over the real one, its
//                        pivot slowly orbiting. A doppelgänger of your game wandering on top of it.
//   2. Homesick pixels— every pixel slowly creeps toward screen-center and back; the frame breathes
//                        inward and outward forever, for no reason.
//   3. Fwoomf          — the frame partially swaps its X and Y and slides back, smearing toward its
//                        across-the-diagonal mirror.
//
// Each shader declares its OWN cbuffer (its own param names); the build reflects it and surfaces those
// names on ScreenSpaceEffect, so we set them inline (`.pivot`/`.blend`, `.amount`) exactly like a built-in
// effect's params — no uniform struct, no byte span. Registration is just the `.hlsl` PATH: the build
// scans this source for it, injects the engine preamble, compiles + embeds the shader, and registers it
// (so this host also keeps the consumer path compiling on every CI platform — SPIR-V + DXIL + MSL — though
// CI never opens the window).
//
// The scene: two scrolling indexed-PNG tile layers (an opaque lower field + a holed upper field whose
// index-0 diamonds reveal it) with a SPRITE layer of four bobbing sprites on top. That sprite layer lets
// the demo prove the headline point: a custom shader is a first-class effect at EITHER attachment point —
//   • WHOLE FRAME (FrameDrawState::postEffects): the finished image warps — background AND sprites.
//   • ONE LAYER (DrawLayer::effect, Layer scope) on the sprite layer: ONLY the sprites warp; the tile
//     background stays put. A sprite layer takes an effect identically to a tile layer.
//
// The effects sample through sampleSource(), so where they reach past the frame edge the behavior is the
// EFFECT's edge policy, not the shader's: Blank (the faithful default) shows nothing there (the layers below
// reveal through); Stretch clamps/smears the border. Down toggles it — the SAME shader honors both, because
// the layer decides.
//
// Run on a dev machine: Z / pad B cycles none → mirror-ghost → homesick → Fwoomf → none; Up toggles the
// target (whole frame / sprite layer only); Down toggles the edge (blank / clamp).

#include <array>
#include <cmath>
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

constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;

// The demo's vocabulary: the effect selectors + the presentation knobs.
enum class Action : std::uint8_t {
    CycleEffect, ToggleTarget, ToggleEdge, Fullscreen, ToggleSampling,
};

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free (SDL3)
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Custom Shader Demo"},
        .window = {.title = "Polyrhythm — custom shader demo (weird useless effects)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Z / pad B cycles the effect; Up (or W) / Down (or S) toggle the target and the edge policy;
    // Backspace / pad Select and Enter / pad Start drive the presentation knobs.
    ActionMap map{
        {Action::CycleEffect,    {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::ToggleTarget,   {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::ToggleEdge,     {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::ToggleSampling, {SDL_SCANCODE_RETURN, PadButton::Start}},
    };
    platform.actions(map);

    // Register all three weird custom shaders BY PATH — that's the whole registration. No uniform struct,
    // no ShaderVariants, no generated-header include, no CMake rule: the build scans this source, sees
    // these .hlsl paths, injects the standard preamble, compiles + embeds each, and registers it by path.
    const PostProcessStageId ghost =
        renderer.registerPostProcessStage("examples/shaders/mirror_ghost.frag.hlsl");
    const PostProcessStageId homesick =
        renderer.registerPostProcessStage("examples/shaders/homesick.frag.hlsl");
    const PostProcessStageId fwoomf =
        renderer.registerPostProcessStage("examples/shaders/fwoomf.frag.hlsl");

    // 0 = none (faithful), 1 = mirror-ghost, 2 = homesick, 3 = Fwoomf. CycleEffect cycles.
    int  mode = 0;
    auto modeName = [](int m) {
        switch (m) {
            case 1:  return "mirror-ghost";
            case 2:  return "homesick pixels";
            case 3:  return "Fwoomf";
            default: return "none (faithful)";
        }
    };

    // WHERE the active effect attaches. A custom shader is a first-class effect, so — exactly like a
    // built-in — it works at EITHER attachment point:
    //   false → a WHOLE-FRAME post-effect (FrameDrawState::postEffects): warps the finished image, so the
    //           background tiles AND the sprites distort together.
    //   true  → a PER-LAYER effect on the SPRITE layer (DrawLayer::effect, Layer scope): warps ONLY that
    //           layer's pixels, so the sprites distort while the tile background stays rock-steady. A
    //           sprite layer takes an effect identically to a tile layer — the effect runs on the layer's
    //           rendered pixels, blind to whether they came from tiles or sprites.
    // Up toggles it.
    bool attachToLayer = false;
    auto targetName    = [](bool layer) { return layer ? "sprite layer only" : "whole frame"; };

    // The active effect's EDGE policy (ScreenSpaceEffect::edge) — proof the edge behavior comes from the
    // EFFECT/layer, not the shader. Blank (default): where an effect samples PAST the frame edge it shows
    // nothing (the backdrop / layers below reveal through). Clamp (Stretch): the border smears. The SAME
    // custom shader (it samples through sampleSource()) honors whichever the layer sets. Down toggles it.
    bool clampEdges = false;
    auto edgeName   = [](bool clamp) { return clamp ? "clamp (Stretch)" : "blank (default)"; };

    // The layer-transparency scene below the effects: one committed indexed PNG uploaded twice (opaque +
    // index-0-transparent), colored through warm/cool palettes, two scrolling tile layers.
    LoadedImage tiles;
    try {
        tiles = loadPng(assetPath("demo_tiles.png"));
    } catch (const std::exception& e) {
        std::printf("demo: could not load demo_tiles.png: %s\n", e.what());
        return 1;
    }
    const AtlasId opaqueAtlas = renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height).atlasId;
    const AtlasId holeAtlas   = renderer.uploadAtlas(tiles.indices.data(), tiles.width, tiles.height, TransparentIndices::of({0})).atlasId;

    const std::array<Rgba8, 4> warm{{ {40, 18, 18}, {180, 70, 60}, {225, 130, 95}, {255, 220, 180} }};
    const std::array<Rgba8, 4> cool{{ {16, 22, 40}, {60, 110, 200}, {110, 175, 240}, {205, 235, 255} }};
    const PaletteId warmPal = renderer.uploadPalette(std::span<const Rgba8>(warm));
    const PaletteId coolPal = renderer.uploadPalette(std::span<const Rgba8>(cool));

    // The same tile pattern on two layers, but each cell names its own sheet + palette directly: the lower
    // (opaque) field draws from `opaqueAtlas` colored warm; the upper (holed) field from `holeAtlas`
    // colored cool. Two cell arrays carry the per-cell handles; the tile index is identical between them.
    std::vector<TileCell> lowerCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> upperCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const auto          idx  = static_cast<std::size_t>(y) * kMapW + x;
            const std::uint16_t tile = static_cast<std::uint16_t>((x % 2) + 2 * (y % 2));
            lowerCells[idx] = TileCell{.atlas = opaqueAtlas, .tile = tile, .palette = warmPal};
            upperCells[idx] = TileCell{.atlas = holeAtlas, .tile = tile,   .palette = coolPal};
        }
    }

    // The SPRITE layer that rides above the tile background (so we can show a per-layer effect on sprites,
    // not just tiles). Four 16×16 sprites that bob slowly; positions are refreshed each frame in the render
    // callback. Sprites reuse the opaque tile atlas — the sprite path treats palette index 0 as transparent
    // automatically, so no separate transparent upload is needed.
    constexpr int       kSpriteCount = 4;
    std::vector<Sprite> sprites{{{.key = "s0"}, {.key = "s1"}, {.key = "s2"}, {.key = "s3"}}};

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::Fullscreen)) {
            platform.window().fullscreen(!platform.window().fullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.window().fullscreen() ? "on" : "off");
        }
        if (in.justPressed(Action::ToggleSampling)) {
            const bool bilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(bilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", bilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::CycleEffect)) {
            mode = (mode + 1) % 4;
            std::printf("[dev] custom effect: %s\n", modeName(mode));
        }
        if (in.justPressed(Action::ToggleTarget)) {
            attachToLayer = !attachToLayer;
            std::printf("[dev] effect target: %s\n", targetName(attachToLayer));
        }
        if (in.justPressed(Action::ToggleEdge)) {
            clampEdges = !clampEdges;
            std::printf("[dev] effect edge: %s\n", edgeName(clampEdges));
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        const int   drift = tick / 6;  // gentle same-direction parallax (~10 px/s); no strobing moiré
        const float t     = static_cast<float>(tick);

        // 1) Build the active custom effect (or a None effect when mode == 0), animated SLOWLY off the
        // Each is a Custom-kind ScreenSpaceEffect that sets ITS OWN shader's params inline —
        // `.pivot`/`.blend` for mirror-ghost, `.amount` for the others — exactly like a built-in's named
        // params. No uniform struct, no byte span: the build reflected each shader's cbuffer and surfaced
        // those names on ScreenSpaceEffect. The SAME effect value is used whether we attach it to the whole
        // frame or to one layer (step 3).
        ScreenSpaceEffect fx{};  // kind == None ⇒ no effect
        if (mode == 1) {  // mirror-ghost: pivot slowly orbits; the ghost fades gently in and out
            fx = ScreenSpaceEffect{
                .kind = ScreenSpaceEffectKind::Custom, .customShader = ghost,
                .pivot = {0.5f + 0.25f * std::cos(t * 0.010f), 0.5f + 0.25f * std::sin(t * 0.010f)},
                .blend = 0.35f + 0.10f * std::sin(t * 0.013f)};
        } else if (mode == 2) {  // homesick: a slow inward/outward breathing pulse
            fx = ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom, .customShader = homesick,
                                   .amount = 0.12f * std::sin(t * 0.020f)};
        } else if (mode == 3) {  // Fwoomf: a slow partial X/Y swap and back
            fx = ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom, .customShader = fwoomf,
                                   .amount = 0.25f * (1.0f + std::sin(t * 0.015f))};
        }
        fx.edge = clampEdges ? DisplacementEdge::Stretch : DisplacementEdge::Blank;  // None: ignored

        // 2) The scene: two scrolling tile background layers + a sprite layer on top.
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

        for (int i = 0; i < kSpriteCount; ++i) {  // bob the sprites (slow, phase-offset; no strobing)
            sprites[i].x       = 16 + i * 40;
            sprites[i].y       = 64 + static_cast<int>(12.0f * std::sin(t * 0.03f + static_cast<float>(i) * 1.2f));
            sprites[i].size    = AssetDimensions{.width = 16, .height = 16};  // reads the whole 16×16 atlas
            sprites[i].tile    = 0;
            sprites[i].atlas   = holeAtlas;   // index 0 is the sprites' transparent background
            sprites[i].palette = warmPal;
        }
        DrawLayer spriteLayer{.key = "bobbingSprites"};
        spriteLayer.z       = 20;  // above the tile background
        spriteLayer.size    = PixelSize{160, 144};
        spriteLayer.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};

        // 3) Attach the effect at the chosen point. Whole-frame → FrameDrawState::postEffects (background +
        //    sprites warp together). Per-layer → the sprite layer's DrawLayer::effects at Layer scope (ONLY
        //    the sprites warp; the tile background stays put). The identical `fx` drives both — the proof
        //    that a custom shader is a first-class effect at every attachment point a built-in is.
        frame.postEffects.clear();
        if (fx.kind != ScreenSpaceEffectKind::None) {
            if (attachToLayer) {
                fx.scope            = ScreenSpaceEffectScope::Layer;
                spriteLayer.effects = {fx};
            } else {
                frame.postEffects.push_back(fx);
            }
        }
        frame.layers.push_back(std::move(spriteLayer));

        renderer.renderFrame(frame);
    });

    std::printf("custom shader demo — three deliberately USELESS consumer shaders (the custom hook's "
                "actual purpose: effects too weird to ever be built-ins), shown as a WHOLE-FRAME "
                "post-effect AND as a PER-LAYER effect on a sprite layer. Close to quit.\n");
    std::printf("[dev] Z / pad B = cycle effect (none / mirror-ghost / homesick / Fwoomf), "
                "Up = target (whole frame / sprite layer only), Down = edge (blank / clamp), "
                "Backspace / pad Select = fullscreen, Enter / pad Start = nearest/bilinear.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
