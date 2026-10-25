// Glow demo — a runnable host that teaches and shows off the built-in GLOW effect
// (ScreenSpaceEffectKind::Glow): an authored-color aura — you pick the color a thing radiates,
// regardless of what color it is.
//
// WHAT IT IS. Glow is Bloom's sibling with the halo's color under YOUR control: a scalar emission mask
// (the silhouette, optionally keyed by brightness) blurs outward and adds back times a chosen tint. Where
// Bloom bleeds the art's own light — a dark shape barely blooms — Glow radiates the tint at full strength
// from dark art too. `fill` (× `fillIntensity`) is the aura color, `radius` the reach in the site's own
// pixels, `threshold` the emission floor (0 = the whole silhouette emits, dark pixels included; higher
// keys the emission on brightness), `intensity` the strength (0 = off, the identity default).
//
// HOW YOU DRIVE IT (the teaching bit). It is a built-in: no shader, no registration — name the kind and
// set the inline fields with plain designated-init, exactly like ColorFill or Bloom:
//
//     Sprite s{ .key = "relic", .x = 64, .y = 56, .atlas = gem, .palette = pal };
//     s.effects = { ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Glow,
//                                      .fill = Rgba8{255, 66, 26, 255},   // ember — the color YOU chose
//                                      .radius = 5.0f, .intensity = 255 } };
//
// It works at EVERY effect site: per-sprite (as here — the halo radius is in the sprite's own art pixels,
// scaled by whatever transform places the sprite), on a DrawLayer::effects, a frame postEffect, or inside
// a layer/frame Region.
//
// THE THREE RELICS (same dark gem art, same palette — ONLY the tint differs):
//   • LEFT — a steady GOLD aura: dark art radiating a color it doesn't contain.
//   • MIDDLE — an EMBER aura whose radius breathes between 3 and 7 on the sim tick.
//   • RIGHT — a VIOLET aura whose intensity pulses, the pick-me-up shimmer a powerup drop wants.
//
// F swaps in a drifting FIELD of glowing relics. Glowing sprites that share a radius are rasterized into
// one shared emission buffer, blurred once and composited once, so the field costs what a single relic
// costs — spend the glow where the scene wants it, not where the frame budget allows. The field MOVES, so
// the cadence is visible: smooth drift is a healthy frame budget, stutter is a blown one. Up / Down change
// the relic count (10..400) and Right / Left the shared aura reach (3..48) — drive both up and watch what
// the cost does.
//
// Space toggles the emission threshold: 0 = the whole silhouette emits (every relic glows entire, dark
// body included); higher = brightness-keyed (only each gem's bright facet still radiates — the same knob
// Bloom keys its highlights with). Backspace = fullscreen. Close to quit.

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
constexpr int kGem = 24;  // a 24x24 diamond-cut gem

enum class Action : std::uint8_t { ThresholdKey, Field, More, Fewer, Wider, Narrower, Fullscreen };

// A 24x24 diamond: |x−c| + |y−c| ≤ 10 is the dark body (index 1), a small bright facet near the top-left
// shoulder (index 2) gives a brightness-keyed threshold something to hold onto; index 0 is the hole.
[[nodiscard]] std::array<std::uint8_t, kGem * kGem> gemArt() {
    std::array<std::uint8_t, kGem * kGem> a{};
    for (int y = 0; y < kGem; ++y)
        for (int x = 0; x < kGem; ++x) {
            const int dist = (x > 11 ? x - 11 : 11 - x) + (y > 11 ? y - 11 : 11 - y);
            std::uint8_t idx = dist <= 10 ? 1 : 0;
            if (idx == 1 && x >= 8 && x <= 9 && y >= 6 && y <= 7) idx = 2;  // the bright facet
            a[static_cast<std::size_t>(y) * kGem + x] = idx;
        }
    return a;
}

// smoothstep 0..1 — the breathe/pulse eases in and out rather than snapping.
[[nodiscard]] float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Glow"},
        .window   = {.title = "Polyrhythm — glow demo (gold / ember / violet auras from the same dark gem)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ThresholdKey, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Field, {SDL_SCANCODE_F, PadButton::FaceEast}},
        {Action::More, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::Fewer, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Wider, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Narrower, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A near-black backdrop — an authored aura reads best against the dark.
    std::array<std::uint8_t, 64> dark{};
    const AtlasId               bgAtlas = renderer.uploadAtlas(dark.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1>  bgPal{{{8, 10, 16}}};
    const PaletteId             bgPalId = renderer.uploadPalette(std::span<const Rgba8>(bgPal));
    const std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH,
                                        TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});

    // One dark art + one palette shared by all three relics — the ONLY difference is each aura's tint.
    // The body is a deep slate a Bloom would barely lift; Glow ignores that and radiates the chosen color.
    const auto    gem      = gemArt();
    const AtlasId gemAtlas =
        renderer.uploadAtlas(gem.data(), kGem, kGem, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 3> gemPal{{{0, 0, 0},          // index 0 — the hole
                                       {30, 34, 44},       // 1 — dark slate body
                                       {235, 240, 255}}};  // 2 — the bright facet (the threshold's key)
    const PaletteId gemPalId = renderer.uploadPalette(std::span<const Rgba8>(gemPal));

    constexpr Rgba8 kGold{255, 204, 82, 255};
    constexpr Rgba8 kEmber{255, 66, 26, 255};
    constexpr Rgba8 kViolet{170, 90, 255, 255};

    // The breathing/pulsing phase (0..1, trianglewave over ~2 s) and the emission-threshold toggle.
    float phase    = 0.0f;
    int   dir      = +1;
    bool  keyed    = false;  // false = whole-silhouette emission (threshold 0); true = brightness-keyed
    bool  field    = false;  // false = the three relics; true = the moving field
    int   count    = 30;     // relics in the field
    float fieldR   = 9.0f;   // their shared aura reach, art px
    float orbit    = 0.0f;   // the field's drift angle, radians — advanced on the tick
    constexpr int kBreatheTicks = 60;  // ~1 s each way at ~59.7 ticks/s

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::ThresholdKey)) {
            keyed = !keyed;
            std::printf("[demo] emission %s\n",
                        keyed ? "brightness-keyed (threshold 140 — only the facets radiate)"
                              : "whole-silhouette (threshold 0 — the dark bodies radiate too)");
        }
        if (in.justPressed(Action::Field)) {
            field = !field;
            std::printf("[demo] %s\n", field ? "the moving field" : "back to the three relics");
        }
        if (field) {
            const int   wasCount = count;
            const float wasR     = fieldR;
            if (in.justPressed(Action::More))     count = count < 400 ? count + 10 : count;
            if (in.justPressed(Action::Fewer))    count = count > 10 ? count - 10 : count;
            if (in.justPressed(Action::Wider))    fieldR = fieldR < 48.0f ? fieldR + 3.0f : fieldR;
            if (in.justPressed(Action::Narrower)) fieldR = fieldR > 3.0f ? fieldR - 3.0f : fieldR;
            if (count != wasCount || fieldR != wasR)
                std::printf("[demo] %d relics, aura reach %.0f — still one raster + one blur + one "
                            "composite, because they share a reach\n", count, static_cast<double>(fieldR));
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        orbit += 0.012f;   // the field drifts every tick, so a dropped frame reads as a stutter
        phase += static_cast<float>(dir) / static_cast<float>(kBreatheTicks);
        if (phase >= 1.0f) { phase = 1.0f; dir = -1; }
        if (phase <= 0.0f) { phase = 0.0f; dir = +1; }
    });

    const int gemY = (kViewH - kGem) / 2;
    const std::array<int, 3> gemX{{20, 68, 116}};

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer backdrop{.key = "backdrop"};
        backdrop.z       = 0;
        backdrop.size    = PixelSize{kViewW, kViewH};
        backdrop.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                       .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(backdrop);

        sprites.clear();
        const std::uint8_t threshold = keyed ? 140 : 0;
        auto relic = [&](std::string key, int x, int y, Rgba8 tint, float radius,
                         std::uint8_t intensity) {
            Sprite s{.key = std::move(key), .x = x, .y = y,
                     .size = AssetDimensions{.width = kGem, .height = kGem},
                     .atlas = gemAtlas, .tile = 0, .palette = gemPalId};
            s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .fill = tint,
                                           .radius = radius, .threshold = threshold,
                                           .intensity = intensity}};
            return s;
        };

        if (field) {
            // A drifting field, all at one reach. They share a reach, so they share an emission buffer:
            // one raster pass, one blur, one composite for the whole field however many there are.
            // Nothing here is a batching API — authoring N glowing sprites the ordinary way is what gets
            // the shared pass. Each relic keeps a STABLE key across frames, which is what lets the engine
            // interpolate its motion between sim ticks; keying by loop index would cross-fade unrelated
            // sprites into each other.
            for (int i = 0; i < count; ++i) {
                const float t   = static_cast<float>(i);
                const float ang = orbit + t * 0.7f;
                const int   x   = static_cast<int>(std::lround(
                    kViewW * 0.5f - kGem * 0.5f + std::cos(ang) * (18.0f + 3.4f * t)));
                const int   y   = static_cast<int>(std::lround(
                    kViewH * 0.5f - kGem * 0.5f + std::sin(ang * 1.3f) * (14.0f + 2.1f * t)));
                const Rgba8 tint = (i % 3 == 0) ? kGold : (i % 3 == 1 ? kEmber : kViolet);
                sprites.push_back(relic("field" + std::to_string(i), x, y, tint, fieldR, 255));
            }
        } else {
            // LEFT — steady gold: one designated-init struct, a color the art nowhere contains.
            sprites.push_back(relic("gold", gemX[0], gemY, kGold, 5.0f, 255));

            // MIDDLE — ember, the radius breathing 3..7 art px on the eased phase. The value is recomputed
            // and resubmitted each frame; the engine never eases effect params for us.
            sprites.push_back(relic("ember", gemX[1], gemY, kEmber, 3.0f + 4.0f * smoothstep(phase), 255));

            // RIGHT — violet, the intensity pulsing 120..255: the aura's strength is just another knob.
            const auto pulse = static_cast<std::uint8_t>(120.0f + 135.0f * smoothstep(phase));
            sprites.push_back(relic("violet", gemX[2], gemY, kViolet, 5.0f, pulse));
        }

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        renderer.renderFrame(frame);
    });

    std::printf("glow demo — three relics, same dark gem art: LEFT a steady gold aura, MIDDLE a breathing "
                "ember, RIGHT a pulsing violet. The colour is AUTHORED (the tint field), not the art's — "
                "that is the difference from Bloom. One built-in: ScreenSpaceEffectKind::Glow.\n");
    std::printf("[demo] Space = whole-silhouette vs brightness-keyed emission, F = the moving field "
                "(one shared emission buffer), Up/Down = relic count, Right/Left = aura reach, "
                "Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
