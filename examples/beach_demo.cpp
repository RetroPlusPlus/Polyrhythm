// Beach scene demo — the headline per-layer screen-space-effect use case: a wavy ocean
// that waves while the sky and sand stay still, plus a rock out in the water being relentlessly beaten
// by the surf. By user request this departs from the usual abstract demo — it composites a recognizable
// scene from a few hand-built indexed tiles (no PNG, no external assets):
//
//   • Sky        (z=0)  — full-viewport solid sky blue. Static.
//   • Rock base  (z=5)  — BENEATH the ocean: the submerged part of the boulder, seen THROUGH the
//                         translucent water. It stays put; the surf beats over it.
//   • Ocean      (z=10) — index-hole atlas (sky shows above the horizon), deep blue + a light-cyan foam
//                         stripe, drawn TRANSLUCENT so the steady submerged rock shows through. Carries a
//                         LAYER-scope (isolated) Vertical RowDisplacement: ONLY the water churns — each
//                         column shifts vertically by sin, so the horizon waterline + foam undulate over
//                         the steady rock. Gentle foam drift via scroll.
//   • Rock crag  (z=15) — ABOVE the ocean: the dry top of the rock, jutting out above the surf. Static.
//   • Sand       (z=20) — index-hole atlas: transparent above, a sandy tile in the bottom rows. Static.
//
// The rock is one boulder split across the ocean's z — a submerged base (z=5, under the translucent
// water) and a dry crag (z=15, above it). Because the ocean's effect is LAYER-scope (isolated), the rock
// stays WHOLE and STILL while only the surf moves over it — the right look for "beaten by waves." (A
// Below-scope ocean would displace the submerged base along with the water, sliding it off the steady
// crag: a rigid object straddling a Below effect's z gets torn between the moving and the still side. The
// fix is exactly this — match the scope to the intent.) A dev toggle adds a content-less BELOW-scope
// shimmer at the very top of the stack, so the WHOLE scene wobbles coherently — that is where the Below
// (adjustment-layer) scope is shown, applied to a flat whole-frame target where there is nothing to tear.
//
// Run it on a dev machine and confirm: the surf churns over the steady, whole rock while the sky and
// sand stay still; toggling the ocean wave off leaves a static beach; toggling the top Below shimmer
// wobbles the whole scene together.
//
// This is one of the runnable example hosts that instantiates SdlPlatform + Renderer in a real run, so
// it keeps the live SDL_GPU pipeline/upload/present path — the per-layer composite + displace-blend, and
// both TILES and SPRITES layers around an effect layer — compiling and linking on every CI platform even
// though CI never opens the window.

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

constexpr int kMapW = 20;  // tilemap dimensions in tiles (covers the 160×144 viewport: 20×18)
constexpr int kMapH = 18;
constexpr int kHorizonRow = 5;   // ocean begins here (sky above); the waterline at y = 40
constexpr int kSandRow    = 13;  // sand begins here (ocean/sky above)

// Atlas tile ids in the shared indexed atlas (5 tiles in a row).
enum Tile : std::uint16_t { TileHole = 0, TileSky = 1, TileOcean = 2, TileSand = 3, TileRock = 4 };

// The demo's vocabulary: the effect toggles + the presentation knobs.
enum class Action : std::uint8_t {
    ToggleOceanWave, ToggleShimmer, Fullscreen, ToggleSampling, WindowScale,
};

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Beach Demo"},
        .window = {.title = "Polyrhythm — beach scene: per-layer effects + wave-beaten rock"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // Up (or W) toggles the ocean wave, Z / pad B the whole-scene shimmer; Backspace / pad Select,
    // Enter / pad Start, and X / pad A drive the presentation knobs.
    ActionMap map{
        {Action::ToggleOceanWave, {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::ToggleShimmer,   {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen,      {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::ToggleSampling,  {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::WindowScale,     {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    // Startup presentation enhancements: the window opened at config.enhancements.windowScale
    // (4×, clamped to the display); set the blit sampler. windowScale is live-toggled below.
    int windowScale = config.enhancements.windowScale;

    // ── Build one indexed atlas: 5 solid/striped/shaded 8×8 tiles laid in a row (40×8 indices). ───
    // Indices: 0 hole · 1 sky · 2 ocean deep · 3 ocean foam · 4 sand · 5 wet sand · 6 rock · 7 rock
    // shadow · 8 rock highlight. Color comes from the palette below; index 0 is the transparent marker
    // (declared transparent on the ocean/sand uploads; the rock tile has no index-0, so it stays solid).
    constexpr int kAtlasTiles = 5;
    constexpr int kAtlasW     = kAtlasTiles * 8;  // 40
    constexpr int kAtlasH     = 8;
    std::array<std::uint8_t, static_cast<std::size_t>(kAtlasW) * kAtlasH> atlasPx{};
    for (int y = 0; y < kAtlasH; ++y) {
        for (int t = 0; t < kAtlasTiles; ++t) {
            for (int x = 0; x < 8; ++x) {
                std::uint8_t idx = 0;
                switch (t) {
                    case TileHole:  idx = 0; break;                            // transparent marker
                    case TileSky:   idx = 1; break;                            // solid sky
                    case TileOcean: idx = (y < 2) ? 3 : 2; break;              // foam stripe over deep
                    case TileSand:  idx = ((x + y) % 5 == 0) ? 5 : 4; break;   // sand with sparse speckle
                    case TileRock:                                             // shaded gray boulder block
                        idx = 6;
                        if (x == 0 || y == 0) idx = 8;                         // top/left highlight
                        if (x == 7 || y == 7) idx = 7;                         // bottom/right shadow
                        break;
                }
                atlasPx[static_cast<std::size_t>(y) * kAtlasW + static_cast<std::size_t>(t) * 8 + x] = idx;
            }
        }
    }

    // Index 0 is "the hole, never shown" for this art, so the sheet that holes it (holeAtlas) serves
    // everything with an index-0 background: ocean, sand, and the rock sprites. The sky tiles are solid
    // (no index 0), so opaqueAtlas — which holes nothing — is fine for them.
    const AtlasId opaqueAtlas = renderer.uploadAtlas(atlasPx.data(), kAtlasW, kAtlasH).atlasId;              // sky tiles
    const AtlasId holeAtlas   = renderer.uploadAtlas(atlasPx.data(), kAtlasW, kAtlasH, TransparentIndices::of({0})).atlasId;

    const std::array<Rgba8, 9> beachPalette{{
        {0, 0, 0},          // 0 hole (never shown)
        {126, 192, 238},    // 1 sky
        {26, 86, 148},      // 2 ocean deep
        {130, 200, 230},    // 3 ocean foam
        {230, 208, 156},    // 4 sand
        {206, 182, 128},    // 5 wet sand
        {120, 120, 130},    // 6 rock
        {72, 72, 84},       // 7 rock shadow
        {170, 170, 182},    // 8 rock highlight
    }};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(beachPalette));

    // ── Three tilemaps (kept alive for the program's duration). Each cell names its own sheet + palette. ─
    auto buildMap = [&](AtlasId atlas, auto tileAt) {
        std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
        for (int y = 0; y < kMapH; ++y) {
            for (int x = 0; x < kMapW; ++x) {
                TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
                c.tile    = tileAt(y);
                c.atlas   = atlas;
                c.palette = pal;
            }
        }
        return cells;
    };
    const std::vector<TileCell> skyCells   = buildMap(opaqueAtlas, [](int) -> std::uint16_t { return TileSky; });
    const std::vector<TileCell> oceanCells = buildMap(holeAtlas, [](int y) -> std::uint16_t {
        return y < kHorizonRow ? TileHole : TileOcean;        // hole above the horizon, ocean below
    });
    const std::vector<TileCell> sandCells  = buildMap(holeAtlas, [](int y) -> std::uint16_t {
        return y < kSandRow ? TileHole : TileSand;            // hole above the beach, sand at the bottom
    });

    // ── The rock, out in the water, split across the ocean's z so the surf passes over its base. ──
    // 8×8 gray boulder sprites stacked into a chunky crag near x≈100. The waterline is y = 40 (the
    // horizon): everything above is the dry crag (composited IN FRONT of the ocean, z=15); everything at
    // and below is the wave-washed base (composited BEHIND the ocean, z=5, so the wavy foam washes over
    // its top and the deep ocean submerges the rest). Index 0 in the sprite path is transparent, but the
    // rock tile has none, so each block is solid. Kept alive for the program's duration.
    const std::array<Sprite, 4> rockCrag{{               // z=15, dry, in front of the surf (y < 40)
        {.key = "crag0", .x = 100, .y = 24, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "crag1", .x = 92,  .y = 32, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "crag2", .x = 100, .y = 32, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "crag3", .x = 108, .y = 32, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
    }};
    const std::array<Sprite, 7> rockBase{{               // z=5, washed + submerged, behind the surf (y ≥ 40)
        {.key = "base0", .x = 92,  .y = 40, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base1", .x = 100, .y = 40, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base2", .x = 108, .y = 40, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base3", .x = 92,  .y = 48, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base4", .x = 100, .y = 48, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base5", .x = 108, .y = 48, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
        {.key = "base6", .x = 100, .y = 56, .atlas = holeAtlas, .tile = TileRock, .palette = pal},
    }};

    bool oceanWave    = true;   // ToggleOceanWave: the headline per-layer (Layer-scope) ocean wobble
    bool belowShimmer = false;  // ToggleShimmer: a whole-scene (Below-scope) shimmer for contrast

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        // Dev toggles (demo only): per-layer effect demonstration + the orthogonal presentation knobs.
        if (in.justPressed(Action::ToggleOceanWave)) {
            oceanWave = !oceanWave;
            std::printf("[dev] ocean wave (Layer scope): %s\n", oceanWave ? "on" : "off");
        }
        if (in.justPressed(Action::ToggleShimmer)) {
            belowShimmer = !belowShimmer;
            std::printf("[dev] whole-scene shimmer (Below scope): %s\n", belowShimmer ? "on" : "off");
        }
        if (in.justPressed(Action::Fullscreen)) {
            platform.window().fullscreen(!platform.window().fullscreen());
            std::printf("[dev] fullscreen: %s\n", platform.window().fullscreen() ? "on" : "off");
        }
        if (in.justPressed(Action::ToggleSampling)) {
            const bool toBilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(toBilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", toBilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::WindowScale)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;
            const PixelSize vp{config.viewport.width, config.viewport.height};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.window().fullscreen()) {
                platform.window().size(PixelSize{vp.width * eff, vp.height * eff});
            }
            std::printf("[dev] window scale: %d×\n", eff);
        }
    });

    // The game owns the draw state; the render callback rebuilds the beach each advance().
    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();

        // Slow, same-direction foam drift (a pixel every few frames) — a calm sea, no strobing.
        const int drift = tick / 8;

        // z=0: sky, full viewport, static.
        DrawLayer sky{.key = "sky"};
        sky.z       = 0;
        sky.size    = PixelSize{160, 144};
        sky.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                  .cells = std::span<const TileCell>(skyCells)};
        frame.layers.push_back(sky);

        // z=5: the rock's submerged base, BENEATH the ocean — it stays put (the ocean's Layer-scope wave
        // moves only the water), shown THROUGH the translucent water as the surf beats over it.
        DrawLayer rockSubmerged{.key = "rockBase"};
        rockSubmerged.z       = 5;
        rockSubmerged.size    = PixelSize{160, 144};
        rockSubmerged.content = SpriteContent{.sprites = std::span<const Sprite>(rockBase)};
        frame.layers.push_back(rockSubmerged);

        // z=10: the ocean — a LAYER-scope (isolated) Vertical RowDisplacement. ONLY the water churns: the
        // rock (base AND crag), the sky, and the sand all stay put while the wavy translucent surf slides
        // over them — a rock BEATEN by the waves, not one dragged along WITH them. (A Below scope here
        // would displace the submerged base too, sliding it off the steady crag — the straddle artifact;
        // for "beaten by waves" you want a stationary rock with the water churning across it.) The ocean
        // is translucent so the steady submerged rock shows through; Blank edge so the wavy top edge
        // reveals the sky/rock below where it pulls inward.
        DrawLayer ocean{.key = "ocean"};
        ocean.z       = 10;
        ocean.size    = PixelSize{160, 144};
        ocean.scroll  = LayerScroll{drift, 0};   // foam drifts gently
        ocean.alpha   = 0.72f;                   // translucent → the steady submerged rock shows through
        ocean.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(oceanCells)};
        if (oceanWave) {
            ocean.effects = {ScreenSpaceEffect{
                .kind      = ScreenSpaceEffectKind::RowDisplacement,
                .amplitude = 5.0f,                              // ±5 viewport px — the surf beats the rock
                .frequency = 2.0f,                              // ~2 crests across the width
                .phase     = static_cast<float>(tick) * 0.008f, // slow drift, no flicker
                .axis      = Axis::Vertical,                    // columns shift vertically → wavy horizon
                .edge      = DisplacementEdge::Blank,           // exposed strip reveals the sky/rock below
                .scope     = ScreenSpaceEffectScope::Layer}};   // isolated: only the water moves
        }
        frame.layers.push_back(ocean);

        // z=15: the rock's dry crag, IN FRONT of the ocean — juts out above the surf, stays still.
        DrawLayer rockCragLayer{.key = "rockCrag"};
        rockCragLayer.z       = 15;
        rockCragLayer.size    = PixelSize{160, 144};
        rockCragLayer.content = SpriteContent{.sprites = std::span<const Sprite>(rockCrag)};
        frame.layers.push_back(rockCragLayer);

        // z=20: sand, static, composited over the ocean's lower edge → the beach.
        DrawLayer sand{.key = "sand"};
        sand.z       = 20;
        sand.size    = PixelSize{160, 144};
        sand.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                   .cells = std::span<const TileCell>(sandCells)};
        frame.layers.push_back(sand);

        // z=30 (dev toggle): a content-less BELOW-scope layer. It draws nothing, but its effect
        // displaces the WHOLE accumulated scene beneath it — sky + rock + ocean + sand wobble
        // coherently. Contrast with the ocean's Layer scope (which moves only the ocean). A steady layer
        // placed ABOVE z=30 would ride still over the shimmer ("wobble the world, keep the HUD steady").
        if (belowShimmer) {
            DrawLayer shimmer{.key = "wholeSceneShimmer"};
            shimmer.z      = 30;
            shimmer.size   = PixelSize{160, 144};
            // content left as the default empty TileContent → draws nothing; only the effect applies.
            shimmer.effects = {ScreenSpaceEffect{
                .kind      = ScreenSpaceEffectKind::RowDisplacement,
                .amplitude = 2.0f,
                .frequency = 2.0f,
                .phase     = static_cast<float>(tick) * 0.004f,
                .axis      = Axis::Horizontal,
                .edge      = DisplacementEdge::Blank,
                .scope     = ScreenSpaceEffectScope::Below}};   // adjustment layer: everything beneath
            frame.layers.push_back(shimmer);
        }

        renderer.renderFrame(frame);
    });

    std::printf("Beach demo — a Layer-scope ocean wave churns over a steady, whole rock "
                "(beating it) while the sky and sand stay still; Z / pad B adds a Below-scope whole-scene shimmer.\n");
    std::printf("[dev] Up = ocean wave on/off, Z / pad B = whole-scene shimmer (Below scope), "
                "Backspace / pad Select = fullscreen, Enter / pad Start = nearest/bilinear, "
                "X / pad A = window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
