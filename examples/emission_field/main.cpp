// Emission-field demo — a runnable host for region BLOOM at a scale a per-layer store could never hold.
//
// WHAT A REGION BLOOM IS. A Bloom inside a sprite REGION is not an additive halo laid over the layer: it is
// the region's SOURCE color, graded under the region's own blend and alpha and confined to shape ∩
// silhouette. So it cannot share a field with anything — it needs THIS sprite's own light, as a color:
//
//     Sprite s{ .key = "emitter", .x = 40, .y = 24, .atlas = orb, .palette = pal };
//     s.regions = { Region{ .key = "core", .shape = ShapePoints::rectangle({0, 0}, 16, 16),
//                           .effects = { ScreenSpaceEffect{ .kind      = ScreenSpaceEffectKind::Bloom,
//                                                           .radius    = 8.0f,
//                                                           .intensity = 255 } } } };
//
// WHAT THIS DEMO SHOWS. Every emitter on screen carries one of those, so every emitter needs a field of its
// own. Each field is sized to the sprite it belongs to and packed into a shared atlas, which is why the
// count on screen can run into the hundreds: capacity is atlas AREA, not a number of fields.
//
// The bar along the bottom is the emitter count. The pale notch near its left end marks EIGHT — walk the
// count past it and every emitter beyond keeps its halo.
//
// The reach is the other half. Fields sharing a reach share a PAGE, and a page blurs once however many
// fields sit on it — so widening the reach widens every halo at the cost of the same single pass. Sweep it
// and watch the halos grow while the frame does not lurch.
//
// THE OTHER STORE. `L` glides a set of Below-scope lenses over the same scene. A Below Bloom is the mirror
// of a region one: it radiates the SCENE's light — everything beneath the lens's layer, emitters included —
// through the lens's own silhouette, and its radius is in viewport pixels rather than art pixels. Every lens
// gets a field the size of its own silhouette out of the same atlas the emitters draw from, so both kinds of
// field pack together and a page's single blur serves whichever sit on it:
//
//     Sprite l{ .key = "lens0", .x = 40, .y = 24, .atlas = orb, .palette = pal };
//     ScreenSpaceEffect bloom{ .kind = ScreenSpaceEffectKind::Bloom, .radius = 8.0f, .intensity = 255 };
//     bloom.scope = ScreenSpaceEffectScope::Below;
//     l.effects = { bloom };
//
// Twelve of them ride above the emitters, and the reach sweep moves theirs too — so a scene of hundreds of
// region fields and a dozen scene-reading ones costs one blur pass per distinct reach between them.
//
// Up / Down = eight more or fewer emitters. Right / Left = wider or narrower reach. L = the lenses.
// Backspace = fullscreen. Close to quit.
//
// Motion advances on the sim tick, so the drift runs the same on any display.

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
constexpr int kOrb  = 16;   // one emitter's art

constexpr int kMinEmitters = 1, kMaxEmitters = 240, kEmitterStep = 8;
constexpr int kMinReach = 1, kMaxReach = 20;
constexpr int kOldCap   = 8;    // what a per-layer field store used to hold — the notch on the bar
constexpr int kBarSpan  = 64;   // emitters the bar's full width represents
constexpr int kLenses   = 12;   // Below-scope lenses the L key glides over the scene

enum class Action : std::uint8_t { More, Fewer, Wider, Narrower, Lenses, Fullscreen };

// A 16x16 ember: a white-hot core stepping out through four warm shades to a hole. One hue family, so the
// bloom reads as the core's own light spreading into its dimmer edge rather than as a ring of another color.
[[nodiscard]] std::array<std::uint8_t, kOrb * kOrb> orbArt() {
    std::array<std::uint8_t, kOrb * kOrb> a{};
    const float c = (kOrb - 1) * 0.5f;
    for (int y = 0; y < kOrb; ++y)
        for (int x = 0; x < kOrb; ++x) {
            const float dx = x - c, dy = y - c;
            const float d  = std::sqrt(dx * dx + dy * dy);
            if (d > 7.2f) continue;                        // outside the disc: a hole
            const int step = static_cast<int>(d / 1.8f);    // 0 at the core, rising outward
            a[static_cast<std::size_t>(y) * kOrb + x] = static_cast<std::uint8_t>(5 - std::min(step, 4));
        }
    return a;
}

// An 8x8 sheet whose top-left 2x2 is lit — the count bar's pips read that corner as a 2x2 sprite. The sheet
// is a whole tile because a sheet narrower than one is not a grid the atlas can carve.
[[nodiscard]] std::array<std::uint8_t, 64> pipArt() {
    std::array<std::uint8_t, 64> a{};
    a[0] = a[1] = a[8] = a[9] = 1;
    return a;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "EmissionField"},
        .window   = {.title = "Polyrhythm — emission fields (region Bloom, many more than eight)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::More, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::Fewer, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Wider, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Narrower, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::Lenses, {SDL_SCANCODE_L, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    std::array<std::uint8_t, 64> bg{};
    bg.fill(1);
    const AtlasId              bgAtlas = renderer.uploadAtlas(bg.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> bgPal{{{0, 0, 0}, {10, 10, 18}}};
    const PaletteId            bgPalId = renderer.uploadPalette(std::span<const Rgba8>(bgPal));
    const std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH,
                                        TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});

    const auto    orb      = orbArt();
    const AtlasId orbAtlas = renderer.uploadAtlas(orb.data(), kOrb, kOrb, TransparentIndices::GameBoy).atlasId;
    // One warm ramp, brightest at the core. Index 0 is the hole.
    const std::array<Rgba8, 6> orbPal{{{0, 0, 0},
                                       {90, 30, 20},       // 1 — the dim outer edge
                                       {150, 60, 25},
                                       {215, 110, 40},
                                       {245, 180, 90},
                                       {255, 240, 210}}};  // 5 — white-hot core
    const PaletteId            orbPalId = renderer.uploadPalette(std::span<const Rgba8>(orbPal));

    const auto    pip      = pipArt();
    const AtlasId pipAtlas = renderer.uploadAtlas(pip.data(), 8, 8, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> pipPal{{{0, 0, 0}, {120, 200, 255}}};
    const std::array<Rgba8, 2> notchPal{{{0, 0, 0}, {255, 240, 200}}};
    const PaletteId            pipPalId   = renderer.uploadPalette(std::span<const Rgba8>(pipPal));
    const PaletteId            notchPalId = renderer.uploadPalette(std::span<const Rgba8>(notchPal));

    // Stable keys, built once. An emitter is reconciled frame to frame by its key, so the interpolator can
    // follow it as it drifts — a key minted per frame would cross-fade unrelated sprites.
    std::vector<std::string> emitterKeys;
    emitterKeys.reserve(kMaxEmitters);
    for (int i = 0; i < kMaxEmitters; ++i) emitterKeys.push_back("emitter" + std::to_string(i));
    std::vector<std::string> lensKeys;
    lensKeys.reserve(kLenses);
    for (int i = 0; i < kLenses; ++i) lensKeys.push_back("lens" + std::to_string(i));
    std::vector<std::string> pipKeys;
    pipKeys.reserve(kBarSpan);
    for (int i = 0; i < kBarSpan; ++i) pipKeys.push_back("pip" + std::to_string(i));

    int  count  = 24;
    int  reach  = 8;
    int  tick   = 0;
    bool lenses = false;

    loop.simTick([&](const InputState& in) {
        ++tick;
        const int wasCount = count, wasReach = reach;
        if (in.justPressed(Action::More))  count = std::min(count + kEmitterStep, kMaxEmitters);
        if (in.justPressed(Action::Fewer)) count = std::max(count - kEmitterStep, kMinEmitters);
        if (in.justPressed(Action::Wider))     reach = std::min(reach + 1, kMaxReach);
        if (in.justPressed(Action::Narrower))  reach = std::max(reach - 1, kMinReach);
        if (in.justPressed(Action::Lenses)) {
            lenses = !lenses;
            std::printf("[demo] %s — %d Below lenses reading the scene's light through their silhouettes\n",
                        lenses ? "lenses on" : "lenses off", lenses ? kLenses : 0);
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        if (count != wasCount)
            std::printf("[demo] %d emitters — %s\n", count,
                        count > kOldCap ? "every one past the eighth would have gone un-bloomed" : "under the old ceiling");
        if (reach != wasReach)
            std::printf("[demo] reach %d art px — one blur pass serves every emitter sharing it\n", reach);
    });

    FrameDrawState      frame;
    std::vector<Sprite> emitters;
    std::vector<Sprite> lensSprites;
    std::vector<Sprite> bar;
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer backdrop{.key = "backdrop"};
        backdrop.z       = 0;
        backdrop.size    = PixelSize{kViewW, kViewH};
        backdrop.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                       .cells = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(backdrop);

        // The emitters drift on slow independent orbits so the scene is alive and every field has to be
        // re-placed each frame — the packer runs per layer, per frame, and this is what exercises it.
        emitters.clear();
        for (int i = 0; i < count; ++i) {
            const float phase = static_cast<float>(tick) * 0.01f + static_cast<float>(i) * 0.7f;
            const int   col   = i % 9, row = (i / 9) % 7;
            const int   x = 6 + col * 17 + static_cast<int>(std::lround(std::sin(phase) * 4.0f));
            const int   y = 4 + row * 18 + static_cast<int>(std::lround(std::cos(phase * 0.8f) * 4.0f));
            Sprite s{.key  = emitterKeys[static_cast<std::size_t>(i)],
                     .x    = x,
                     .y    = y,
                     .size = AssetDimensions{.width = kOrb, .height = kOrb},
                     .atlas = orbAtlas,
                     .tile  = 0,
                     .palette = orbPalId};
            s.regions = {Region{.key     = "core",
                                .shape   = ShapePoints::rectangle({0.0f, 0.0f}, static_cast<float>(kOrb),
                                                                  static_cast<float>(kOrb)),
                                .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                                              .radius    = static_cast<float>(reach),
                                                              .intensity = 255}}}};
            emitters.push_back(s);
        }
        DrawLayer field{.key = "emitters"};
        field.z       = 10;
        field.size    = PixelSize{kViewW, kViewH};
        field.content = SpriteContent{.sprites = std::span<const Sprite>(emitters)};
        frame.layers.push_back(field);

        // The lenses: the same art as an emitter, but its pixels are only the coverage mask — a lens draws no
        // art of its own. Each reads the scene beneath its layer, so it picks up the emitters' light and
        // spreads it through its own disc. They glide on paths of their own so their fields are re-placed
        // every frame alongside the emitters'.
        lensSprites.clear();
        if (lenses) {
            for (int i = 0; i < kLenses; ++i) {
                const float phase = static_cast<float>(tick) * 0.006f + static_cast<float>(i) * 0.9f;
                // Across the full width, but held to the band the emitters occupy — a lens over empty
                // backdrop has no light to gather, so it would read as nothing at all.
                const int   x = kViewW / 2 - kOrb / 2 + static_cast<int>(std::lround(std::sin(phase) * 62.0f));
                const int   y = 24 + static_cast<int>(std::lround(std::cos(phase * 0.6f + i) * 22.0f));
                Sprite l{.key     = lensKeys[static_cast<std::size_t>(i)],
                         .x       = x,
                         .y       = y,
                         .size    = AssetDimensions{.width = kOrb, .height = kOrb},
                         .atlas   = orbAtlas,
                         .tile    = 0,
                         .palette = orbPalId};
                // A Glow rather than a Bloom: the scene's light keys WHERE the halo appears, but its color
                // is the one authored here. A cool tint over a field of warm embers is what makes the lens
                // legible — a Bloom would re-radiate the embers' own orange on top of orange.
                ScreenSpaceEffect glow{.kind      = ScreenSpaceEffectKind::Glow,
                                       .fill      = Rgba8{.r = 90, .g = 190, .b = 255, .a = 255},
                                       .radius    = static_cast<float>(reach),
                                       .threshold = 40,
                                       .intensity = 255};
                glow.scope = ScreenSpaceEffectScope::Below;
                l.effects  = {glow};
                lensSprites.push_back(l);
            }
        }
        DrawLayer lensLayer{.key = "lenses"};
        lensLayer.z       = 15;
        lensLayer.size    = PixelSize{kViewW, kViewH};
        lensLayer.content = SpriteContent{.sprites = std::span<const Sprite>(lensSprites)};
        frame.layers.push_back(lensLayer);

        // The count bar: one pip per two emitters, with the eighth marked. Its whole job is to make "this
        // used to stop at eight" visible without reading a doc.
        bar.clear();
        const int pips  = std::min((count + 1) / 2, kBarSpan);
        const int notch = kOldCap / 2;
        for (int i = 0; i < pips; ++i) {
            const bool marks = i == notch;
            bar.push_back(Sprite{.key  = pipKeys[static_cast<std::size_t>(i)],
                                 .x    = 14 + i * 2,
                                 .y    = kViewH - 8,
                                 .size = AssetDimensions{.width = 2, .height = 2},
                                 .atlas = pipAtlas,
                                 .tile  = 0,
                                 .palette = marks ? notchPalId : pipPalId});
        }
        DrawLayer meter{.key = "meter"};
        meter.z       = 20;
        meter.size    = PixelSize{kViewW, kViewH};
        meter.content = SpriteContent{.sprites = std::span<const Sprite>(bar)};
        frame.layers.push_back(meter);

        renderer.renderFrame(frame);
    });

    std::printf("emission-field demo — every orb carries a region Bloom, so every orb needs a field of its "
                "own. Capacity is atlas area, not a count of fields.\n");
    std::printf("[demo] Up/Down = more or fewer emitters, Right/Left = wider or narrower reach, "
                "L = Below lenses, Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
