// Custom-emission demo — a game-registered custom shader joins the built-ins' emission grammar.
//
// A custom stage becomes an emission consumer with ONE declaration line in its .hlsl —
// `// @retropp:emission`. The engine then extracts a glow field for the stage, blurs it by the effect's
// `.radius`, and hands it back to the shader through `sampleEmission(uv)`. There are two ways to fill that
// field, and this demo shows both:
//
//   • AUTHORED (examples/custom_emission/neon_emission.frag.hlsl) — the shader defines
//     `float4 emission(float2 uv)` beside main(). That body writes the field, so it can emit a signal the
//     stock brightpass cannot: here, a blue-dominant MASK that lights the deep-blue neon and ignores the
//     far-brighter warm lamps.
//   • STOCK (examples/custom_emission/scene_bloom.frag.hlsl) — the declaration line alone, no emission()
//     body. The engine fills the field with the built-in brightpass at the effect's `.threshold` (a bloom of
//     the source's own bright light). One line, no shader math — a Bloom obtained by declaration.
//
// Demand rides `.radius` / `.threshold` on the ScreenSpaceEffect, exactly like a built-in Glow/Bloom: no new
// fields, no API call, and no CMake rule beyond the per-target shader scan retropp_add_example already
// applies. `sampleEmission(uv)` takes the same uv `sampleSource(uv)` does, at every site.
//
// TWO SITES, both shown:
//   • FRAME (FrameDrawState::postEffects) — a whole-frame custom emission stage over the composited image.
//     Space toggles it between the AUTHORED neon shader and the STOCK bloom shader, so the difference reads on
//     one scene: authored lights the blue neon, stock lights the warm lamps.
//   • BELOW LENS (a Sprite whose effect scope is Below) — the STOCK bloom shader over the scene beneath the
//     lens silhouette, blooming the lamps it drifts across. The one-line case at a sprite lens.
//
// Up/Down widen/narrow the reach — radius is nearly free (the field is blurred, not gathered), so a wide glow
// costs about what a narrow one does. Backspace = fullscreen. Close to quit.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"  // TransparentIndices
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 192, kViewH = 144;
constexpr int kMapW = 24, kMapH = 18;  // 8×8 backdrop tiles covering the viewport

enum class Action : std::uint8_t { SwapFrameShader, Wider, Narrower, Fullscreen };

// A filled disc of index 1 on an index-0 (transparent) field: |p − center| ≤ r.
[[nodiscard]] std::vector<std::uint8_t> disc(int n, float r) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(n) * n, 0);
    const float c = (n - 1) * 0.5f;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float dx = x - c, dy = y - c;
            if (dx * dx + dy * dy <= r * r) a[static_cast<std::size_t>(y) * n + x] = 1;
        }
    return a;
}

// A ring of index 1 on an index-0 (transparent) field: innerR ≤ |p − center| ≤ outerR.
[[nodiscard]] std::vector<std::uint8_t> ring(int n, float outerR, float innerR) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(n) * n, 0);
    const float c = (n - 1) * 0.5f;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float dx = x - c, dy = y - c;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= outerR * outerR && d2 >= innerR * innerR)
                a[static_cast<std::size_t>(y) * n + x] = 1;
        }
    return a;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "CustomEmission"},
        .window   = {.title = "Polyrhythm — custom emission demo (authored neon glow vs. stock-bloomed lamps)"},
        .viewport = ViewportResolution{kViewW, kViewH}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::SwapFrameShader, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Wider, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::Narrower, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // Register the two custom stages BY PATH — the build scans this source for the .hlsl literals, compiles
    // each to this platform's bytecode, sees the `// @retropp:emission` line, and marks the stage an emission
    // consumer. The handles are what a Custom effect names in `.customShader`.
    const PostProcessStageId neonShader =
        renderer.registerPostProcessStage("examples/custom_emission/neon_emission.frag.hlsl");
    const PostProcessStageId bloomShader =
        renderer.registerPostProcessStage("examples/custom_emission/scene_bloom.frag.hlsl");

    // ── Scene art (built in code — no external assets) ──
    // A near-black backdrop; an authored glow reads best against the dark.
    std::array<std::uint8_t, 64> darkTile{};
    const AtlasId               bgAtlas = renderer.uploadAtlas(darkTile.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1>  bgPal{{{8, 10, 16}}};
    const PaletteId             bgPalId = renderer.uploadPalette(std::span<const Rgba8>(bgPal));
    const std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH,
                                        TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});

    // The NEON: a deep-blue ring. Vivid blue, but LOW luminance (Rec.601 weights blue least), so the stock
    // brightpass gates it out — the authored emission() body is the only way to light it.
    constexpr int      kNeon = 52;
    const auto         neonArt = ring(kNeon, kNeon * 0.5f - 1.0f, kNeon * 0.5f - 6.0f);
    const AtlasId      neonAtlas =
        renderer.uploadAtlas(neonArt.data(), kNeon, kNeon, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> neonPal{{{0, 0, 0}, {40, 70, 255}}};  // 0 = hole, 1 = deep blue
    const PaletteId            neonPalId = renderer.uploadPalette(std::span<const Rgba8>(neonPal));

    // The LAMPS: small warm discs. HIGH luminance, so the stock brightpass lights these and the authored
    // blue-mask body ignores them — the two shaders pick opposite content from one scene.
    constexpr int      kLamp = 18;
    const auto         lampArt = disc(kLamp, kLamp * 0.5f - 2.0f);
    const AtlasId      lampAtlas =
        renderer.uploadAtlas(lampArt.data(), kLamp, kLamp, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> lampPal{{{0, 0, 0}, {255, 236, 156}}};  // 0 = hole, 1 = warm lamp
    const PaletteId            lampPalId = renderer.uploadPalette(std::span<const Rgba8>(lampPal));

    // The LENS: a filled DISC whose silhouette is the Below-scope effect's footprint — a filled disc blooms
    // the whole scene under it (a thin ring would bloom only its rim). At Below scope the sprite grades the
    // scene beneath and does not draw its own pixels, so a second plain RING sprite rides on top as a visible
    // frame that shows where the lens sits.
    constexpr int      kLens = 60;
    const auto         lensArt = disc(kLens, kLens * 0.5f - 1.0f);
    const AtlasId      lensAtlas =
        renderer.uploadAtlas(lensArt.data(), kLens, kLens, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> lensPal{{{0, 0, 0}, {90, 120, 180}}};  // the silhouette carrier (not composited)
    const PaletteId            lensPalId = renderer.uploadPalette(std::span<const Rgba8>(lensPal));
    const auto         rimArt = ring(kLens, kLens * 0.5f - 1.0f, kLens * 0.5f - 3.0f);
    const AtlasId      rimAtlas =
        renderer.uploadAtlas(rimArt.data(), kLens, kLens, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> rimPal{{{0, 0, 0}, {150, 180, 230}}};  // 0 = hole, 1 = the visible rim
    const PaletteId            rimPalId = renderer.uploadPalette(std::span<const Rgba8>(rimPal));

    bool  useNeon = true;   // which shader the FRAME stage runs: true = authored neon, false = stock bloom
    float reach   = 15.0f;  // the glow reach both stages share, viewport px
    float orbit   = 0.0f;   // the lens's drift angle, advanced on the tick

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::SwapFrameShader)) {
            useNeon = !useNeon;
            std::printf("[demo] frame stage: %s\n",
                        useNeon ? "AUTHORED neon_emission — the emission() body lights the blue neon"
                                : "STOCK scene_bloom — the brightpass lights the warm lamps");
        }
        if (in.justPressed(Action::Wider))    reach = reach < 40.0f ? reach + 3.0f : reach;
        if (in.justPressed(Action::Narrower)) reach = reach > 3.0f ? reach - 3.0f : reach;
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        orbit += 0.011f;  // the lens drifts every tick, so a dropped frame reads as a stutter
    });

    // Lamp row + neon placement (static).
    const int neonX = (kViewW - kNeon) / 2, neonY = 10;
    const std::array<int, 3> lampX{{28, (kViewW - kLamp) / 2, kViewW - kLamp - 28}};
    const int lampY = 92;

    FrameDrawState      frame;
    std::vector<Sprite> content;
    std::vector<Sprite> lensSprites;
    loop.renderLoop([&]() {
        frame.layers.clear();
        content.clear();

        DrawLayer backdrop{.key = "backdrop"};
        backdrop.z       = 0;
        backdrop.size    = PixelSize{kViewW, kViewH};
        backdrop.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                       .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(backdrop);

        // The neon ring and the three warm lamps — plain sprites, no effects of their own, in the CONTENT
        // layer. The lens below reads the scene beneath ITS layer, so the content it lenses lives here, under it.
        content.push_back(Sprite{.key = "neon", .x = neonX, .y = neonY,
                                 .size = AssetDimensions{.width = kNeon, .height = kNeon},
                                 .atlas = neonAtlas, .tile = 0, .palette = neonPalId});
        for (int i = 0; i < 3; ++i)
            content.push_back(Sprite{.key = "lamp" + std::to_string(i), .x = lampX[static_cast<std::size_t>(i)],
                                     .y = lampY,
                                     .size = AssetDimensions{.width = kLamp, .height = kLamp},
                                     .atlas = lampAtlas, .tile = 0, .palette = lampPalId});

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(content)};
        frame.layers.push_back(actors);

        // The BELOW LENS — a drifting sprite in its OWN layer above the content, carrying the STOCK bloom stage
        // at Below scope. A Below-scope effect grades the composited scene beneath the sprite's LAYER, confined
        // to the silhouette: here it blooms the lamps the lens drifts across. `.threshold` keys the brightpass
        // on the lamps' brightness, `.radius` sets the reach. The lens grades the scene and draws none of its own
        // pixels, so the plain RING sprite beside it is the visible frame that marks where the lens sits.
        lensSprites.clear();
        const int lensX = static_cast<int>(kViewW * 0.5f - kLens * 0.5f + 44.0f * std::cos(orbit));
        const int lensY = static_cast<int>(lampY + kLamp * 0.5f - kLens * 0.5f + 14.0f * std::sin(orbit * 1.3f));
        Sprite    lens{.key = "lens", .x = lensX, .y = lensY,
                       .size = AssetDimensions{.width = kLens, .height = kLens},
                       .atlas = lensAtlas, .tile = 0, .palette = lensPalId};
        ScreenSpaceEffect belowBloom{.kind = ScreenSpaceEffectKind::Custom, .customShader = bloomShader,
                                     .scope = ScreenSpaceEffectScope::Below, .radius = reach, .threshold = 180};
        lens.effects = {belowBloom};
        lensSprites.push_back(lens);
        lensSprites.push_back(Sprite{.key = "lens-rim", .x = lensX, .y = lensY,
                                     .size = AssetDimensions{.width = kLens, .height = kLens},
                                     .atlas = rimAtlas, .tile = 0, .palette = rimPalId});

        DrawLayer lensLayer{.key = "lens-layer"};
        lensLayer.z       = 20;
        lensLayer.size    = PixelSize{kViewW, kViewH};
        lensLayer.content = SpriteContent{.sprites = std::span<const Sprite>(lensSprites)};
        frame.layers.push_back(lensLayer);

        // The FRAME custom emission stage — a whole-frame Custom effect over the composited image. Space
        // swaps which registered shader runs; both are emission consumers, so both get an extracted, blurred
        // field they read through sampleEmission(). `.threshold` is the stock brightpass floor (only the STOCK
        // shader reads it — the authored body writes the field itself); `.radius` is the shared reach.
        frame.postEffects.clear();
        frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,
                                                      .customShader = useNeon ? neonShader : bloomShader,
                                                      .radius = reach, .threshold = 200});

        renderer.renderFrame(frame);
    });

    std::printf("custom-emission demo — a game shader joins the built-ins' emission grammar with one line:\n"
                "  // @retropp:emission   (declares the stage an emission consumer)\n"
                "The engine extracts + blurs a field and hands it back through sampleEmission(uv). Fill it two\n"
                "ways: define float4 emission(float2 uv) to AUTHOR a signal (neon_emission lights the blue\n"
                "neon a luminance brightpass misses), or declare only and let the STOCK brightpass fill it\n"
                "(scene_bloom lights the warm lamps). Demand rides .radius / .threshold — no new fields.\n");
    std::printf("[demo] Space = swap the FRAME shader (authored neon / stock bloom), Up/Down = reach, "
                "Backspace = fullscreen. The drifting LENS carries the stock bloom at Below scope. Close to "
                "quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
