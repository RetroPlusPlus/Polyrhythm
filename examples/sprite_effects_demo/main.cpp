// Sprite-effects demo — a runnable host for `Sprite::effects` and `Sprite::regions`. Two 16×16 heroes sit
// side by side over a scrolling scene — a FLAT solid disc (left) and a TEXTURED shaded ball (right) —
// carrying the SAME effect stack, so each effect shows on flat and textured art at once (the sheen is the
// clearest contrast: it keys off the ball's luminance and rides flat over the disc). Toggle key by key:
//
//   • SHEEN (always on) — a `Gleam` chain effect whose `sweep` animates off the tick, so a luminance-keyed
//     highlight sweeps across the hero's bright body and fades at its rim. A whole-silhouette chain effect.
//   • FLASH (A) — a white `ColorFill` region over the whole silhouette, its `alpha` pulsing: a damage
//     flash. Empty region shape ⇒ the whole sprite.
//   • TINT (B) — a `Multiply` `ColorFill` region confined to a quad-space rectangle over the hero's lower
//     half: a shadow that darkens only part of the silhouette.
//   • HOLE (X) — a `Transparency` region over a quad-space circle at the hero's center: the scene behind
//     shows through a punched hole.
//   • CHARGE (C) — a game-registered CUSTOM shader running inline on the sprite: sampleSource() reads the
//     sprite's own art, and the shader remaps it toward an electric blue-white energy look keyed off its
//     luminance — the warm hero visibly turns cool and blazing, a per-pixel recolor no built-in kind does. It
//     runs first in the chain, so the Gleam sheen then rides over the recolored result — a custom step and a
//     built-in composing in one pass. Registered by path, exactly like a layer / Below custom effect.
//
// A third sprite below the heroes shows the DISPLACING kinds — a striped tile whose art is re-read at a
// shifted position:
//   • WAVE (W) — cycles the striped sprite off / RowDisplacement (the bars shear into a wave) / Ripple (they
//     push radially from the center). The displacement is in the sprite's own art px; a crest pulled past the
//     static quad renders in the inflated footprint, and where the art is pulled aside the exposed strip is
//     transparent so the scrolling scene shows through.
//
// A fourth sprite shows a BELOW-SCOPE effect — the scene-facing counterpart. Instead of re-reading its OWN
// art, a Below-scope effect distorts / grades the composited SCENE beneath the sprite, confined to the
// silhouette — a refraction lens. The disc's art is the coverage mask and is not drawn (an opaque mask gives a
// full-strength lens), so the visible result inside is the transformed scene; off the silhouette the scene is
// untouched. `.scope = Below` is the only difference from the corresponding Layer-scope effect (scene instead
// of art). The one disc cycles the whole Below surface:
//   • LENS mode 1 — a built-in `Ripple` (its `center` / `amplitude` are VIEWPORT px on the Below path, so the
//     center tracks the lens's on-screen position).
//   • LENS mode 2 — the SAME `chargeShader` custom shader as the heroes, but `.scope = Below`: its
//     sampleSource() now reads the SCENE, so a game shader recolors the scene beneath into an energy field
//     through the silhouette.
//   • LENS mode 3 — a Below `ColorFill` inside a centered REGION: only the region's shape ∩ the silhouette
//     grades the scene, so the lens's core recolors and its outer ring shows the scene untouched.
//   • LENS mode 4 — a whole-silhouette Below `ColorFill` with a feathered `Transparency` region at the core:
//     the porthole scales the lens strength back toward zero, revealing the untouched scene through the grade.
//
// An N-LENS field (N key) fills the frame with many small disc lenses that ALL carry the same built-in Below
// Ripple. They share one below pipeline, so the whole field draws in ONE instanced pass — the below pass count
// tracks the authored pipeline mix, never the sprite count.
//
// Layer-scope effects evaluate inline in the sprite fragment — no extra passes. Below-scope sprites draw
// through a scene-reading pipeline into a scratch composited over the accumulator — one pass per below-pipeline
// per layer (a built-in run and a custom run are two passes), never per sprite. Parameters are per-tick data,
// so the flash pulse, the sheen sweep, and the wave/refraction phase are recomputed and resubmitted each tick
// (the interpolator never eases effect params). The pixel-exact math is the ctest suite's job (the
// sprite_effects_*.cpp tests vs the shaders); this is the live GPU sanity check.
//
// Motion advances on the sim tick, so it runs the same on any display. A = flash, B = tint, X = hole,
// W = wave, C = charge, L = cycle lens (off/ripple/custom/region/transparency, Below-scope), N = N-lens field,
// Backspace = fullscreen; close to quit.

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

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;

enum class Action : std::uint8_t { Flash, Tint, Hole, Wave, Charge, Lens, Nlens, Fullscreen };

// A 16×16 solid disc: index 1 inside radius 7, index 0 (a GameBoy hole) outside — a round, FLAT sprite
// (one uniform color, no internal texture).
[[nodiscard]] std::array<std::uint8_t, 16 * 16> discArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            a[static_cast<std::size_t>(y) * 16 + x] = (dx * dx + dy * dy <= 7.0f * 7.0f) ? 1 : 0;
        }
    return a;
}

// A 16×16 shaded ball: a lit sphere with a highlight up-and-left (index 3 brightest → 1 darkest), index 0
// (a GameBoy hole) outside radius 7. The internal luminance banding is what a luminance-keyed effect like
// Gleam rides — the sheen catches the highlight and skips the shadowed rim, which a flat disc can't show.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> shadedBallArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const float dx = x - 7.5f, dy = y - 7.5f;
            if (dx * dx + dy * dy > 7.0f * 7.0f) { a[static_cast<std::size_t>(y) * 16 + x] = 0; continue; }
            const float hd = std::sqrt((x - 5.0f) * (x - 5.0f) + (y - 5.0f) * (y - 5.0f));  // dist to highlight
            const std::uint8_t idx = hd < 3.0f ? 3 : hd < 6.5f ? 2 : 1;
            a[static_cast<std::size_t>(y) * 16 + x] = idx;
        }
    return a;
}

// A 16×16 opaque striped tile — alternating vertical bars (indices 1 and 2, every 2 px), no hole. A horizontal
// RowDisplacement shears the bars into a wave; a Ripple pushes them radially. Where a crest pulls the art
// aside, the exposed strip is transparent (the default Blank edge) so the scene shows through — the
// infinite-transparent-field domain the displacing kinds work in.
[[nodiscard]] std::array<std::uint8_t, 16 * 16> stripeArt() {
    std::array<std::uint8_t, 16 * 16> a{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            a[static_cast<std::size_t>(y) * 16 + x] = static_cast<std::uint8_t>(1 + (x / 2) % 2);
    return a;
}

// An opaque round disc mask of side `d` (a multiple of 8): index 1 inside, index 0 (a GameBoy hole) outside.
// The coverage silhouette for the Below-scope lens — its art is the mask, never drawn, so a large disc gives
// a big, legible refraction of the scene beneath.
[[nodiscard]] std::vector<std::uint8_t> discMask(int d) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(d) * static_cast<std::size_t>(d), 0);
    const float c = (d - 1) * 0.5f, rr = d * 0.5f - 1.0f;
    for (int y = 0; y < d; ++y)
        for (int x = 0; x < d; ++x) {
            const float dx = x - c, dy = y - c;
            a[static_cast<std::size_t>(y) * d + x] = (dx * dx + dy * dy <= rr * rr) ? 1 : 0;
        }
    return a;
}

// A lamp of side `d`: a hot radial core falling off to a warm rim. Index 3 core → 2 mid → 1 rim → 0 hole.
// The Below Bloom / Glow lens radiates the SCENE's light, so the scene needs light with STRUCTURE — a
// localized source with falloff, not an evenly-lit field. An evenly-lit field emits evenly, and an even
// emission is a flat tint however wide the blur.
[[nodiscard]] std::vector<std::uint8_t> lampArt(int d) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(d) * static_cast<std::size_t>(d), 0);
    const float c = (d - 1) * 0.5f, r = d * 0.5f - 1.0f;
    for (int y = 0; y < d; ++y)
        for (int x = 0; x < d; ++x) {
            const float dx = x - c, dy = y - c;
            const float t  = std::sqrt(dx * dx + dy * dy) / r;   // 0 at the core, 1 at the rim
            a[static_cast<std::size_t>(y) * d + x] =
                t > 1.0f ? 0 : t > 0.66f ? 1 : t > 0.33f ? 2 : 3;
        }
    return a;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Effects Demo"},
        .window   = {.title = "Polyrhythm — sprite-effects demo (sheen / flash / tint / hole)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Flash, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Tint, {SDL_SCANCODE_B, PadButton::FaceEast}},
        {Action::Hole, {SDL_SCANCODE_X, PadButton::FaceWest}},
        {Action::Wave, {SDL_SCANCODE_W, PadButton::FaceNorth}},
        {Action::Charge, {SDL_SCANCODE_C, PadButton::ShoulderL}},
        {Action::Lens, {SDL_SCANCODE_L, PadButton::ShoulderR}},
        {Action::Nlens, {SDL_SCANCODE_N, PadButton::Start}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A game-registered custom shader that runs INLINE on a sprite: the same registration a layer / Below
    // custom effect uses. The sprite draws through this shader's sprite pipeline; sampleSource() reads the
    // sprite's own art. The handle rides the effect's `.customShader`.
    const PostProcessStageId chargeShader =
        renderer.registerPostProcessStage("examples/sprite_effects_demo/shaders/sprite_charge.frag.hlsl");

    // The opaque scene the sprite's hole reveals: four distinct jewel tones — clearly different hues so the
    // punched-through hole shows obvious color, but deeper in value so a full screen of them is easy on the
    // eyes.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    const AtlasId sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> scenePal{{{170, 60, 80}, {45, 120, 130}, {180, 140, 55}, {95, 70, 150}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    const std::vector<TileCell> sceneCells(static_cast<std::size_t>(kMapW) * kMapH,
                                           TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId});

    const auto disc = discArt();
    const auto ball = shadedBallArt();
    const AtlasId discAtlas = renderer.uploadAtlas(disc.data(), 16, 16, TransparentIndices::GameBoy).atlasId;
    const AtlasId ballAtlas = renderer.uploadAtlas(ball.data(), 16, 16, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> heroPal{{{0, 0, 0}, {255, 150, 30}}};  // flat disc: one bright-orange body
    // Shaded ball: index 0 is the hole; 1→3 are dark rim → mid → bright highlight, a lit sphere's luminance.
    const std::array<Rgba8, 4> ballPal{{{0, 0, 0}, {140, 70, 15}, {215, 120, 35}, {255, 225, 175}}};
    const PaletteId heroPalId = renderer.uploadPalette(std::span<const Rgba8>(heroPal));
    const PaletteId ballPalId = renderer.uploadPalette(std::span<const Rgba8>(ballPal));

    const auto    stripe      = stripeArt();
    const AtlasId stripeAtlas = renderer.uploadAtlas(stripe.data(), 16, 16).atlasId;  // opaque — no transparent index
    const std::array<Rgba8, 3> stripePal{{{0, 0, 0}, {40, 120, 160}, {170, 210, 230}}};  // idx 0 unused
    const PaletteId stripePalId = renderer.uploadPalette(std::span<const Rgba8>(stripePal));

    // The lens: the same round disc art as the coverage MASK for a Below-scope effect. A Below sprite draws
    // no art (it's a lens) — only its alpha shapes the lens, so an OPAQUE disc gives a full-strength
    // refraction over the whole silhouette. The color is unused; index 1 = the lens body (alpha 255), index
    // 0 = a GameBoy hole (outside the disc).
    const std::array<Rgba8, 2> lensPal{{{0, 0, 0}, {255, 255, 255, 255}}};
    const PaletteId lensPalId = renderer.uploadPalette(std::span<const Rgba8>(lensPal));

    // The lamps the Bloom / Glow lens radiates (lens mode 5). They live in their OWN layer beneath the lens's,
    // because a Below lens reads the accumulator composited under its layer — a light in the lens's own layer
    // is invisible to it. Their core is near-white (luminance ≈ 0.98) while the busiest scene tile reaches
    // only ≈ 0.56, so a threshold between the two keys the whole background out and leaves the lamps as the
    // only emitters: the halo then has a source and a falloff instead of being an even wash.
    constexpr int kLampD = 24;
    const std::vector<std::uint8_t> lamp = lampArt(kLampD);
    const AtlasId lampAtlas =
        renderer.uploadAtlas(lamp.data(), kLampD, kLampD, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 4> lampPal{{{0, 0, 0}, {180, 90, 30}, {255, 190, 90}, {255, 250, 230}}};
    const PaletteId lampPalId = renderer.uploadPalette(std::span<const Rgba8>(lampPal));
    constexpr int kLensD = 80;   // a big lens, so the refraction of the scene beneath is legible
    const std::vector<std::uint8_t> lensMask = discMask(kLensD);
    const AtlasId lensAtlas = renderer.uploadAtlas(lensMask.data(), kLensD, kLensD, TransparentIndices::GameBoy).atlasId;

    bool flash = false, tint = false, charge = false, nlens = false;
    int  holeMode = 0;  // 0 = off, 1 = TransparentInside (a hole), 2 = TransparentOutside (a porthole)
    int  waveMode = 1;  // 0 = off, 1 = RowDisplacement, 2 = Ripple (on the striped sprite)
    // Below-scope lens: 0 off / 1 Ripple / 2 charge custom / 3 region grade / 4 transparency reveal /
    // 5 bloom + glow (two emission fields on one lens)
    int  lensMode = 0;
    constexpr int kNlensCount = 24;   // the N-flat field: N built-in-ripple lenses in ONE below pass
    auto holeLabel = [&]() {
        return holeMode == 1 ? "hole (inside)" : holeMode == 2 ? "porthole (outside)" : "off";
    };
    auto waveLabel = [&]() {
        return waveMode == 1 ? "row-displace" : waveMode == 2 ? "ripple" : "off";
    };
    auto lensLabel = [&]() {
        switch (lensMode) {
            case 1:  return "ripple (built-in)";
            case 2:  return "charge (custom shader)";
            case 3:  return "region grade (confined)";
            case 4:  return "transparency reveal";
            case 5:  return "bloom + glow (two emission fields)";
            default: return "off";
        }
    };
    auto announce = [&]() {
        std::printf("sprite-effects demo — sheen always on; flash %s, tint %s, hole %s, wave %s, charge %s, "
                    "lens %s, N-lens %s. A = flash, B = tint, X = cycle hole (off/inside/outside), W = cycle "
                    "wave (off/row/ripple), C = charge (custom shader), L = cycle lens (off/ripple/custom/region/"
                    "transparency — the Below-scope surface over the SCENE beneath), N = %d-lens field (one "
                    "below pass), Backspace = fullscreen; close to quit.\n",
                    flash ? "ON" : "off", tint ? "ON" : "off", holeLabel(), waveLabel(), charge ? "ON" : "off",
                    lensLabel(), nlens ? "ON" : "off", kNlensCount);
    };
    announce();

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Flash))  { flash = !flash;   announce(); }
        if (in.justPressed(Action::Tint))   { tint = !tint;     announce(); }
        if (in.justPressed(Action::Hole))   { holeMode = (holeMode + 1) % 3; announce(); }
        if (in.justPressed(Action::Wave))   { waveMode = (waveMode + 1) % 3; announce(); }
        if (in.justPressed(Action::Charge)) { charge = !charge; announce(); }
        if (in.justPressed(Action::Lens))   { lensMode = (lensMode + 1) % 6; announce(); }
        if (in.justPressed(Action::Nlens))  { nlens = !nlens; announce(); }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    std::vector<Region> heroRegions;
    loop.renderLoop([&]() {
        const double t = static_cast<double>(loop.tickCount());
        frame.layers.clear();

        DrawLayer scene{.key = "scene"};
        scene.z       = 0;
        scene.size    = PixelSize{kViewW, kViewH};
        scene.scroll  = LayerScroll{static_cast<int>(t) / 16 % (kMapW * 8), 0};
        scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(sceneCells)};
        frame.layers.push_back(scene);

        // The lamp layer — z 5, between the scene and the actors, so the lens (an actor) sees it. Present
        // only for the Bloom / Glow lens, which is the mode that has anything to say about them.
        std::vector<Sprite> lamps;
        if (lensMode == 5) {
            for (int i = 0; i < 3; ++i) {
                lamps.push_back(Sprite{.key   = std::string("lamp-") + std::to_string(i),
                                       .x     = 28 + i * 44,
                                       .y     = 72 - kLampD / 2,
                                       .size  = AssetDimensions{.width = kLampD, .height = kLampD},
                                       .atlas = lampAtlas, .tile = 0, .palette = lampPalId});
            }
            DrawLayer lights{.key = "lights"};
            lights.z       = 5;
            lights.size    = PixelSize{kViewW, kViewH};
            lights.content = SpriteContent{.sprites = std::span<const Sprite>(lamps)};
            frame.layers.push_back(lights);
        }

        const int heroY = 64 + static_cast<int>(4.0 * std::sin(t * 0.024));  // a shared gentle vertical bob

        // The shared effect stack — built once, applied to BOTH sprites so each toggle shows on flat AND
        // textured art at once. A Gleam sheen whose crest sweeps across the sprite (sweep animates off the
        // tick); the scrolling scene behind provides the motion the hole reveals.
        const float sweep = static_cast<float>(0.5 + 0.5 * std::sin(t * 0.03));
        // The hero chain. When charge is on, the custom shader runs FIRST (it re-reads the raw art), then the
        // Gleam sheen rides over the recolored result — a chain step and a custom step composing in one pass.
        std::vector<ScreenSpaceEffect> heroChain;
        if (charge) {
            ScreenSpaceEffect g{.kind = ScreenSpaceEffectKind::Custom, .customShader = chargeShader};
            g.charge = static_cast<float>(0.7 + 0.3 * std::sin(t * 0.05));  // breathes 0.4 → 1.0 off the tick
            heroChain.push_back(g);
        }
        heroChain.push_back(
            ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .sweep = sweep, .width = 0.5f, .gain = 1.6f});

        heroRegions.clear();
        if (flash) {  // a white damage flash over the whole silhouette, alpha pulsing
            const float a = static_cast<float>(0.4 + 0.4 * std::sin(t * 0.12));
            heroRegions.push_back(Region{.key = "flash",
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                       .fill = Rgba8{255, 255, 255, 255}}},
                                         .alpha = a});
        }
        if (tint) {  // a Multiply shadow on the lower half (quad-space rectangle y ∈ [8,16] of a 16px sprite)
            heroRegions.push_back(Region{.key = "tint",
                                         .shape = ShapePoints::rectangle(Point{0, 8}, 16, 8),
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                       .fill = Rgba8{90, 90, 130, 255}}},
                                         .blend = BlendMode::Multiply});
        }
        if (holeMode != 0) {  // a see-through circle at the center (quad-space); the scene shows through it
            const StencilMode mode = holeMode == 1 ? StencilMode::TransparentInside   // punch the circle out
                                                   : StencilMode::TransparentOutside;  // keep only the circle
            heroRegions.push_back(Region{.key = "hole",
                                         .shape = ShapePoints::circle(Point{8, 8}, 3.5f),
                                         .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Transparency,
                                                                       .stencil = mode}}});
        }

        // FLAT disc (left) and TEXTURED shaded ball (right), same effects on each. Keys are stable + unique.
        auto styled = [&](const char* key, int x, AtlasId atlas, PaletteId pal) {
            Sprite s{.key = key, .x = x, .y = heroY, .size = AssetDimensions::Snes16x16,
                     .atlas = atlas, .tile = 0, .palette = pal};
            s.effects = heroChain;
            s.regions = heroRegions;
            return s;
        };
        sprites.clear();
        sprites.push_back(styled("flat", 40, discAtlas, heroPalId));
        sprites.push_back(styled("ball", 96, ballAtlas, ballPalId));

        // The displacing striped sprite below — its art is re-read at a shifted within-sprite position; the
        // phase advances off the tick to animate. RowDisplacement shears the bars into a wave; Ripple pushes
        // them radially from the center.
        if (waveMode != 0) {
            const float phase = static_cast<float>(t * 0.02);
            const ScreenSpaceEffect disp =
                waveMode == 1
                    ? ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f,
                                        .frequency = 2.0f, .phase = phase, .axis = Axis::Horizontal}
                    : ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 3.0f,
                                        .frequency = 3.0f, .phase = phase, .center = Point{8, 8}, .decay = 1.2f};
            Sprite water{.key = "water", .x = 72, .y = 108, .size = AssetDimensions::Snes16x16,
                         .atlas = stripeAtlas, .tile = 0, .palette = stripePalId};
            water.effects = {disp};
            sprites.push_back(water);
        }

        // The Below-scope lens — an opaque disc that drifts across the scene, its effect scoped Below so it
        // distorts / grades the SCENE beneath (not its own art): the disc silhouette is the coverage, the
        // visible result inside is the transformed scene, and the disc's own art is NOT drawn (a lens is its
        // mask). Off the silhouette the scene is untouched. The one disc cycles the whole Below surface:
        //   • mode 1 — a built-in Ripple (its `center` / `amplitude` are VIEWPORT px on the Below path, so the
        //     center tracks the lens's on-screen position); `.scope = Below` is the only difference from the
        //     striped sprite's own-art displacement above.
        //   • mode 2 — the SAME `chargeShader` custom shader as the heroes, but `.scope = Below`: its
        //     sampleSource() now reads the SCENE, so it recolors the scene beneath into an energy field
        //     through the silhouette — a game shader driving the lens.
        //   • mode 3 — a Below-scope ColorFill inside a centered REGION: the scene recolors only where the
        //     region covers ∩ the silhouette, so the lens's outer ring shows the untouched scene and its core
        //     grades — region-confined scene grading.
        //   • mode 4 — a whole-silhouette Below ColorFill over the scene, then a Below-scope Transparency
        //     region (a feathered circle, TransparentInside) that scales the lens strength back toward zero at
        //     the core: a soft porthole revealing the untouched scene through the graded lens.
        //   • mode 5 — a Below Bloom and a Below Glow together, both keyed to the scene's own bright content:
        //     the Bloom spreads the scene's color, the wide amber Glow pulses an authored aura over it. Their
        //     reaches differ, so they are two distinct emission fields — the renderer prepares both before the
        //     lens draws and the one draw samples each from its own layer of the shared field array.
        if (lensMode != 0) {
            const int lensX = static_cast<int>(80.0 - kLensD / 2 + 30.0 * std::sin(t * 0.012));  // drifts round center
            const int lensY = 72 - kLensD / 2;
            const Point lensCore{kLensD * 0.5f, kLensD * 0.5f};   // the lens's own center, quad-space px
            Sprite lensSprite{.key = "lens", .x = lensX, .y = lensY,
                              .size = AssetDimensions{.width = kLensD, .height = kLensD},
                              .atlas = lensAtlas, .tile = 0, .palette = lensPalId};
            if (lensMode == 1) {
                ScreenSpaceEffect ripple{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 8.0f,
                                         .frequency = 7.0f, .phase = static_cast<float>(t * 0.02),
                                         .center = Point{static_cast<float>(lensX + kLensD / 2),
                                                         static_cast<float>(lensY + kLensD / 2)},
                                         .decay = 0.5f};  // viewport px, on the lens
                ripple.scope = ScreenSpaceEffectScope::Below;
                lensSprite.effects = {ripple};
            } else if (lensMode == 2) {
                ScreenSpaceEffect custom{.kind = ScreenSpaceEffectKind::Custom, .customShader = chargeShader};
                custom.charge = static_cast<float>(0.7 + 0.3 * std::sin(t * 0.03));  // breathes the recolor in
                custom.scope  = ScreenSpaceEffectScope::Below;
                lensSprite.effects = {custom};
            } else if (lensMode == 3) {
                ScreenSpaceEffect fill{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{70, 200, 230, 255}};
                fill.scope = ScreenSpaceEffectScope::Below;
                lensSprite.regions = {Region{.key = "lens-core",
                                             .shape   = ShapePoints::circle(lensCore, kLensD * 0.28f),
                                             .effects = {fill}}};
            } else if (lensMode == 4) {  // a whole-silhouette scene grade with a feathered Transparency porthole
                ScreenSpaceEffect fill{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{70, 200, 230, 255}};
                fill.scope = ScreenSpaceEffectScope::Below;
                lensSprite.effects = {fill};
                ScreenSpaceEffect reveal{.kind = ScreenSpaceEffectKind::Transparency,
                                         .stencil = StencilMode::TransparentInside, .feather = 14.0f};
                reveal.scope = ScreenSpaceEffectScope::Below;
                lensSprite.regions = {Region{.key = "lens-reveal",
                                             .shape   = ShapePoints::circle(lensCore, kLensD * 0.28f),
                                             .effects = {reveal}}};
            } else {  // mode 5 — the lamps' light, bloomed and glowed through the silhouette
                // Two emission steps at different reaches on ONE lens: a tight Bloom spreading the lamps'
                // own color, and a wide amber Glow keyed to the same light. Each samples its own layer of
                // the shared emission field, so both land in the one draw the lens already costs.
                //
                // `threshold` 170 is the load-bearing number: it sits above the brightest scene tile
                // (luminance ≈ 0.56) and below the lamp cores (≈ 0.98), so the background contributes
                // nothing and the halo is the lamps alone. Drop it under ~143 and every pixel emits, which
                // reads as a flat tint over the lens rather than as light.
                ScreenSpaceEffect bloom{.kind = ScreenSpaceEffectKind::Bloom, .radius = 7.0f,
                                        .threshold = 170, .intensity = 255};
                bloom.scope = ScreenSpaceEffectScope::Below;
                ScreenSpaceEffect glow{.kind          = ScreenSpaceEffectKind::Glow,
                                       .fill          = Rgba8{255, 160, 50, 255},
                                       .fillIntensity = 2.2f,   // > 1 — an HDR-hot aura through the float16 chain
                                       .radius        = 22.0f,
                                       .threshold     = 170,
                                       .intensity     = static_cast<std::uint8_t>(
                                           170 + static_cast<int>(85.0 * std::sin(t * 0.03)))};
                glow.scope = ScreenSpaceEffectScope::Below;
                lensSprite.effects = {bloom, glow};
            }
            sprites.push_back(lensSprite);
        }

        // N-flat proof — a field of many small opaque-disc lenses, every one carrying the SAME built-in Below
        // Ripple. They share one below pipeline, so the whole field renders in ONE instanced pass: the below
        // pass count tracks the authored pipeline mix, never the sprite count. Each drifts slowly on the tick.
        if (nlens) {
            for (int i = 0; i < kNlensCount; ++i) {
                const float fi = static_cast<float>(i);
                const int   nx = 12 + (i % 6) * 26 + static_cast<int>(6.0 * std::sin(t * 0.02 + fi));
                const int   ny = 16 + (i / 6) * 30 + static_cast<int>(4.0 * std::cos(t * 0.017 + fi));
                ScreenSpaceEffect ripple{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 4.0f,
                                         .frequency = 6.0f, .phase = static_cast<float>(t * 0.02),
                                         .center = Point{static_cast<float>(nx + 8), static_cast<float>(ny + 8)},
                                         .decay = 0.6f};
                ripple.scope = ScreenSpaceEffectScope::Below;
                Sprite l{.key = std::string("nlens-") + std::to_string(i), .x = nx, .y = ny,
                         .size = AssetDimensions::Snes16x16, .atlas = discAtlas, .tile = 0, .palette = heroPalId};
                l.effects = {ripple};
                sprites.push_back(l);
            }
        }
        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
