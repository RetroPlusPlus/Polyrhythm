// Bloom demo — a runnable host that teaches and shows off the built-in BLOOM effect
// (ScreenSpaceEffectKind::Bloom): a threshold-blur-add glow that makes bright content radiate a soft halo.
//
// WHAT IT IS. Bloom blurs the pixels brighter than a threshold and adds the blur back over the source, so
// a bright shape glows PAST its own edges — an aura no scaled copy or fill can produce (a copy behind an
// opaque icon only ever shows as a rim; bloom radiates from the art itself, holes and concavities intact).
// `radius` is the halo reach in the site's own pixels, `threshold` the luminance floor (0 = everything
// blooms), `intensity` the strength (0 = off, the identity default; 255 = full).
//
// HOW YOU DRIVE IT (the teaching bit). It is a built-in: you never write a shader or register anything —
// name the kind and set the inline fields with plain designated-init, exactly like ColorFill or Gleam:
//
//     Sprite s{ .key = "drop", .x = 64, .y = 56, .atlas = icon, .palette = pal };
//     s.effects = { ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Bloom,
//                                      .radius = 5.0f, .intensity = 255 } };
//
// It works at EVERY effect site. Here it is a per-sprite effect (the halo radius is in the sprite's own
// art pixels, and the sprite's footprint grows so the halo is never clipped); put the same struct on a
// DrawLayer::effects, a frame postEffect, or inside a Region to bloom a layer, the whole frame, or one
// shape. Space toggles a whole-frame Bloom here so the starfield's bright stars catch a glow too.
//
// THE THREE MAGNETS (same concave horseshoe art, same palette, side by side):
//   • LEFT — the control. No effect: the bare icon.
//   • MIDDLE — a static Bloom (radius 5, intensity 255): the steady aura. The mouth of the horseshoe
//     stays open — the halo hugs the silhouette, it never fills concavities.
//   • RIGHT — a breathing aura: the radius eases between 3 and 7 on the sim tick (slow, about two
//     seconds per cycle), the pick-me-up pulse a powerup drop wants.
//
// Motion advances on the sim tick, so the breathe runs the same on any display. Space = toggle the
// whole-frame bloom, Backspace = fullscreen. Close to quit.
//
// Bloom's halo is the art's OWN light — dark art barely blooms. For an aura in a color you CHOOSE
// (dark art radiating gold), see the sibling ScreenSpaceEffectKind::Glow and examples/glow.

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
constexpr int kIcon = 24;  // a 24x24 horseshoe magnet — concave on purpose (the mouth must stay open)

enum class Action : std::uint8_t { FrameBloom, Fullscreen };

// A 24x24 horseshoe: a thick bar across the top, two legs down the sides, the mouth open through the
// bottom edge (index 0 — a GameBoy hole, contiguous with the outside). Index 1 is the body, index 2 a
// bright rim along the top bar so the bloom has a hot spot to catch.
[[nodiscard]] std::array<std::uint8_t, kIcon * kIcon> horseshoeArt() {
    std::array<std::uint8_t, kIcon * kIcon> a{};
    for (int y = 0; y < kIcon; ++y)
        for (int x = 0; x < kIcon; ++x) {
            const bool body = (y < 8) || (x < 8) || (x >= 16);
            std::uint8_t idx = body ? 1 : 0;
            if (body && y < 3) idx = 2;  // the bright rim
            a[static_cast<std::size_t>(y) * kIcon + x] = idx;
        }
    return a;
}

// smoothstep 0..1 — the breathe eases in and out rather than snapping between radii.
[[nodiscard]] float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Bloom"},
        .window   = {.title = "Polyrhythm — bloom demo (control / steady aura / breathing aura)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::FrameBloom, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A near-black starfield: mostly index 0 (deep space), a few bright star texels the whole-frame bloom
    // catches when toggled on.
    std::array<std::uint8_t, 64> space{};
    space[9] = 1; space[38] = 1; space[52] = 2;  // two dim stars + one hot one per tile
    const AtlasId              bgAtlas = renderer.uploadAtlas(space.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> bgPal{{{8, 10, 26}, {150, 160, 200}, {255, 250, 235}}};
    const PaletteId            bgPalId = renderer.uploadPalette(std::span<const Rgba8>(bgPal));
    const std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH,
                                        TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});

    // One art + one palette shared by all three magnets — the ONLY difference between them is the effect.
    const auto    icon      = horseshoeArt();
    const AtlasId iconAtlas =
        renderer.uploadAtlas(icon.data(), kIcon, kIcon, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 3> iconPal{{{0, 0, 0},          // index 0 — the hole (the open mouth)
                                        {196, 84, 40},      // 1 — copper body
                                        {255, 214, 170}}};  // 2 — the bright rim (the bloom's hot spot)
    const PaletteId iconPalId = renderer.uploadPalette(std::span<const Rgba8>(iconPal));

    // The breathing aura's phase (0..1, trianglewave over ~2 s) and the whole-frame bloom toggle.
    float phase      = 0.0f;
    int   dir        = +1;
    bool  frameBloom = false;
    constexpr int kBreatheTicks = 60;  // ~1 s each way at ~59.7 ticks/s

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::FrameBloom)) {
            frameBloom = !frameBloom;
            std::printf("[demo] whole-frame bloom %s\n", frameBloom ? "ON (the stars glow)" : "off");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        phase += static_cast<float>(dir) / static_cast<float>(kBreatheTicks);
        if (phase >= 1.0f) { phase = 1.0f; dir = -1; }
        if (phase <= 0.0f) { phase = 0.0f; dir = +1; }
    });

    const int iconY = (kViewH - kIcon) / 2;
    const std::array<int, 3> iconX{{20, 68, 116}};

    FrameDrawState      frame;
    std::vector<Sprite> sprites;
    loop.renderLoop([&]() {
        frame.layers.clear();
        frame.postEffects.clear();

        DrawLayer backdrop{.key = "backdrop"};
        backdrop.z       = 0;
        backdrop.size    = PixelSize{kViewW, kViewH};
        backdrop.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                       .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(backdrop);

        sprites.clear();
        auto magnet = [&](const char* key, int x) {
            return Sprite{.key = key, .x = x, .y = iconY,
                          .size = AssetDimensions{.width = kIcon, .height = kIcon},
                          .atlas = iconAtlas, .tile = 0, .palette = iconPalId};
        };

        // LEFT — control: the bare icon.
        sprites.push_back(magnet("control", iconX[0]));

        // MIDDLE — the steady aura: one designated-init field pair. The mouth stays open.
        Sprite steady = magnet("steady", iconX[1]);
        steady.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                            .radius = 5.0f, .intensity = 255}};
        sprites.push_back(steady);

        // RIGHT — the breathing aura: the radius rides the eased phase (3..7 art px). The value is
        // recomputed and resubmitted each tick; the engine never eases effect params for us.
        const float breatheR = 3.0f + 4.0f * smoothstep(phase);
        Sprite breathe = magnet("breathe", iconX[2]);
        breathe.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                             .radius = breatheR, .intensity = 255}};
        sprites.push_back(breathe);

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        // The same struct at the FRAME site: a gentle whole-frame bloom, thresholded so only the bright
        // stars and the icons' hot rims catch it — the scene glows, the deep space stays black.
        if (frameBloom)
            frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                                          .radius = 2.0f, .threshold = 128,
                                                          .intensity = 200});

        renderer.renderFrame(frame);
    });

    std::printf("bloom demo — three magnets, same art: LEFT bare (control), MIDDLE a steady aura, RIGHT a "
                "breathing one; Space toggles a whole-frame bloom over the starfield. One built-in: "
                "ScreenSpaceEffectKind::Bloom.\n");
    std::printf("[demo] Space = whole-frame bloom on/off, Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
