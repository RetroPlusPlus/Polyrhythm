// Swirl demo — a runnable host that teaches and shows off the built-in SWIRL effect
// (ScreenSpaceEffectKind::Swirl): an angular twist about a center. The whirlpool.
//
// WHAT IT IS. Swirl is Ripple's angular sibling. Ripple pushes each sample ALONG the radius from a center
// (concentric rings, a dropped stone); Swirl carries it AROUND the center. Content inside `radius` of
// `center` rotates — the full turn at the exact center, easing smoothly to nothing at the rim — and content
// outside the disc is untouched, pixel for pixel. `amplitude` is that turn in DEGREES (the engine's angle
// unit, the same one Transform::rotation takes), `phase` ADDS to it, and positive turns the content
// clockwise.
//
// HOW YOU DRIVE IT (the teaching bit). It is a built-in: no shader, no registration — name the kind and set
// the inline fields with plain designated-init, exactly like Ripple or Bloom:
//
//     frame.postEffects.push_back(ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::Swirl,
//                                                    .amplitude = 140.0f,        // degrees of turn
//                                                    .center = Point{80, 72},    // viewport px
//                                                    .radius = 56.0f });         // the disc, viewport px
//
// Advancing `phase` a few degrees per tick spins the vortex, which is what this demo does. The effect works
// at every site: a frame postEffect (as here), a DrawLayer::effects step, inside a layer/frame Region, on a
// sprite's chain (where center and radius are the sprite's OWN art pixels and the footprint grows so a
// twisted crest is never clipped), and as a Below-scope sprite lens (the scene twists through the
// silhouette).
//
// THE POND. One 160x144 water field — waves, sparks, lily pads, stones — carved into 8x8 tiles and laid
// back down in order, so the layer reconstructs the field seamlessly. The detail is the point: a twist you
// cannot see is a twist you cannot tune.
//
// Space toggles the whirlpool. Tab flips the direction (watch the sign: positive is clockwise). Backspace =
// fullscreen. Close to quit.

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
constexpr int kTile  = 8;
constexpr int kMapW  = kViewW / kTile;  // 20
constexpr int kMapH  = kViewH / kTile;  // 18

enum class Action : std::uint8_t { Toggle, Flip, Fullscreen };

// The pond, authored as ONE 160x144 field and sliced by the upload — never as a 8x8 motif tiled flat,
// which would give the twist nothing to smear. Palette indices:
//   0 deep water · 1 mid water · 2 light water · 3 shimmer · 4 spark · 5 lily · 6 lily highlight · 7 stone
[[nodiscard]] std::vector<std::uint8_t> pondField() {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(kViewW) * kViewH, 0);
    auto put = [&](int x, int y, std::uint8_t v) {
        if (x >= 0 && x < kViewW && y >= 0 && y < kViewH)
            a[static_cast<std::size_t>(y) * kViewW + x] = v;
    };
    auto disc = [&](int cx, int cy, int r, std::uint8_t v) {
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                const int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r) put(x, y, v);
            }
    };

    // Water: two crossing wave bands give broad shading a rotation visibly shears.
    for (int y = 0; y < kViewH; ++y)
        for (int x = 0; x < kViewW; ++x) {
            const int band = ((x + y * 2) / 11 + (x - y) / 17) % 3;
            put(x, y, static_cast<std::uint8_t>(band == 0 ? 0 : band == 1 ? 1 : 2));
        }
    // Shimmer streaks — short bright dashes on a slanted lattice.
    for (int y = 3; y < kViewH; y += 9)
        for (int x = (y / 9 % 2) * 6; x < kViewW; x += 13)
            for (int k = 0; k < 5; ++k) put(x + k, y + k / 3, 3);
    // Sparks — single hot pixels on a coarser lattice; they draw the eye to the rotation.
    for (int y = 6; y < kViewH; y += 18)
        for (int x = 9; x < kViewW; x += 21) put(x, y, 4);
    // Lily pads with highlights, and a few stones — hard-edged shapes whose deformation is unmistakable.
    const std::array<std::array<int, 3>, 5> pads{{{26, 30, 9}, {124, 38, 7}, {36, 110, 8},
                                                  {132, 106, 10}, {80, 20, 6}}};
    for (const auto& p : pads) {
        disc(p[0], p[1], p[2], 5);
        disc(p[0] - p[2] / 3, p[1] - p[2] / 3, p[2] / 3, 6);
    }
    const std::array<std::array<int, 3>, 3> stones{{{18, 72, 5}, {142, 70, 4}, {80, 128, 6}}};
    for (const auto& s : stones) disc(s[0], s[1], s[2], 7);
    return a;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Swirl"},
        .window   = {.title = "Polyrhythm — swirl demo (a whirlpool over a lily pond)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Toggle, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Flip, {SDL_SCANCODE_TAB, PadButton::FaceEast}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // One field, carved into 8x8 cells by the upload; laying tile (row * kMapW + col) back at (col, row)
    // reconstructs it seamlessly.
    const std::vector<std::uint8_t> field = pondField();
    const AtlasId pondAtlas = renderer.uploadAtlas(field.data(), kViewW, kViewH).atlasId;
    const std::array<Rgba8, 8> pondPal{{{12, 38, 78},      // 0 deep water
                                        {22, 62, 116},     // 1 mid water
                                        {38, 96, 158},     // 2 light water
                                        {96, 168, 214},    // 3 shimmer
                                        {180, 240, 255},   // 4 spark
                                        {36, 132, 74},     // 5 lily
                                        {96, 196, 108},    // 6 lily highlight
                                        {150, 140, 128}}}; // 7 stone
    const PaletteId pondPalId = renderer.uploadPalette(std::span<const Rgba8>(pondPal));

    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int row = 0; row < kMapH; ++row)
        for (int col = 0; col < kMapW; ++col)
            cells[static_cast<std::size_t>(row) * kMapW + col] =
                TileCell{.atlas   = pondAtlas,
                         .tile    = static_cast<std::uint16_t>(row * kMapW + col),
                         .palette = pondPalId};

    // The whirlpool. `spin` is the per-tick advance in DEGREES — the vortex turns a little further every
    // tick and `phase` carries the accumulation.
    constexpr float kBaseTurn = 150.0f;  // degrees of turn at the center
    constexpr float kRadius   = 56.0f;   // the disc, viewport px
    constexpr float kSpin     = 1.5f;    // degrees per sim tick
    float phase     = 0.0f;
    bool  whirling  = true;
    float direction = +1.0f;  // positive = the content turns clockwise

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Toggle)) {
            whirling = !whirling;
            std::printf("[demo] whirlpool %s\n", whirling ? "on" : "off (an exact identity — a zero turn "
                                                                   "leaves every pixel alone)");
        }
        if (in.justPressed(Action::Flip)) {
            direction = -direction;
            std::printf("[demo] direction %s\n",
                        direction > 0.0f ? "clockwise (positive degrees)" : "counter-clockwise (negative)");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        phase += kSpin;
        if (phase >= 360.0f) phase -= 360.0f;
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        frame.postEffects.clear();

        DrawLayer pond{.key = "pond"};
        pond.z       = 0;
        pond.size    = PixelSize{kViewW, kViewH};
        pond.content = TileContent{.widthInTiles  = kMapW,
                                   .heightInTiles = kMapH,
                                   .cells         = std::span<const TileCell>(cells)};
        frame.layers.push_back(pond);

        if (whirling) {
            // The whole effect: a kind, a turn in degrees, a center, a disc. `amplitude` and `phase` SUM,
            // so the accumulating phase spins the vortex while the base turn sets its depth.
            ScreenSpaceEffect whirl{.kind = ScreenSpaceEffectKind::Swirl};
            whirl.amplitude = direction * kBaseTurn;
            whirl.phase     = direction * phase;
            whirl.center    = Point{kViewW / 2.0f, kViewH / 2.0f};
            whirl.radius    = kRadius;
            frame.postEffects.push_back(whirl);
        }

        renderer.renderFrame(frame);
    });

    std::printf("swirl demo — a whirlpool over a lily pond. Content inside the disc rotates (most at the "
                "centre, none at the rim); everything outside is untouched. One built-in: "
                "ScreenSpaceEffectKind::Swirl, with the turn in DEGREES.\n");
    std::printf("[demo] Space = toggle the whirlpool, Tab = flip direction, Backspace = fullscreen. "
                "Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
