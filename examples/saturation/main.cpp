// Color-saturation demo — a runnable host that teaches and shows off the built-in COLOR SATURATION effect
// (ScreenSpaceEffectKind::ColorSaturation): a cross-channel grade that pulls each pixel toward its own
// luminance, draining color toward grayscale.
//
// WHAT IT IS. Saturation is a color operation ColorFill can't express: ColorFill is per-channel (scale +
// offset + mix toward one solid color), but desaturation pulls every channel toward the SAME target — the
// pixel's OWN brightness. saturation 255 leaves the pixel exactly as-is (the identity, the default), 0 makes
// it gray, and values between drain it partway.
//
// HOW YOU DRIVE IT (the teaching bit). It is a built-in: you never write a shader or register anything — you
// name the kind and set ONE inline field with plain designated-init, exactly like ColorFill or Gleam. The
// amount is a uint8, the same 0..255 surface as a color channel (255 = full, 0 = gray):
//
//     Sprite s{ .key = "hero", .x = 64, .y = 56, .atlas = ball, .palette = pal };
//     s.effects = { ScreenSpaceEffect{ .kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 128 } };
//
// It works at EVERY effect site — here it is a per-sprite effect; put the same struct on a DrawLayer::effects,
// a frame postEffect, or inside a Region to desaturate a layer, the whole frame, or one shape.
//
// THE THREE BALLS (same art, same palette, side by side):
//   • LEFT — the control. No effect at all: full color.
//   • MIDDLE — a static ColorSaturation at 0: fully grayscale, every frame.
//   • RIGHT — an animated grade. Its saturation eases between 255 (full color) and 0 (gray); Space toggles
//     the direction, so one press drains the color out over about a second and the next press floods it
//     back. The game owns the timeline (the eased value is recomputed each tick and written into the effect);
//     the engine owns the shader.
//
// The beach-ball art (four saturated wedges — red / green / blue / yellow) makes the drain obvious: at full
// saturation the wedges are vivid; at 0 they collapse to four grays of different brightness.
//
// Motion advances on the sim tick, so the ease runs the same on any display. Space = toggle the right ball's
// drain, Backspace = fullscreen. Close to quit.
//
// A dev drives the window.

#include <array>
#include <cmath>
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
constexpr int kBall = 32;  // a 32x32 ball, big enough that the wedge colors (and their drain) read clearly

enum class Action : std::uint8_t { Toggle, Fullscreen };

// A 32x32 beach ball: four angular wedges (indices 1..4) inside radius 15, index 0 (a GameBoy hole) outside.
// The four saturated wedge colors make the desaturation obvious — vivid hues collapse to distinct grays.
[[nodiscard]] std::array<std::uint8_t, kBall * kBall> beachBallArt() {
    std::array<std::uint8_t, kBall * kBall> a{};
    const float c = (kBall - 1) * 0.5f, r = kBall * 0.5f - 1.0f;
    for (int y = 0; y < kBall; ++y)
        for (int x = 0; x < kBall; ++x) {
            const float dx = x - c, dy = y - c;
            if (dx * dx + dy * dy > r * r) { a[static_cast<std::size_t>(y) * kBall + x] = 0; continue; }
            const bool right = dx >= 0.0f, lower = dy >= 0.0f;  // four quadrant wedges
            const std::uint8_t idx = right ? (lower ? 4 : 1) : (lower ? 3 : 2);
            a[static_cast<std::size_t>(y) * kBall + x] = idx;
        }
    return a;
}

// smoothstep 0..1 — eased so the drain accelerates and decelerates rather than moving at a constant rate.
[[nodiscard]] float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Saturation"},
        .window   = {.title = "Polyrhythm — colour-saturation demo (control / greyscale / animated drain)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Toggle, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // A dark neutral backdrop so all three balls — including the grayscale one — stand clear of the background.
    std::array<std::uint8_t, 64> bg{};
    bg.fill(1);
    const AtlasId          bgAtlas = renderer.uploadAtlas(bg.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> bgPal{{{0, 0, 0}, {28, 30, 40}}};
    const PaletteId        bgPalId = renderer.uploadPalette(std::span<const Rgba8>(bgPal));
    const std::vector<TileCell> bgCells(static_cast<std::size_t>(kMapW) * kMapH,
                                        TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});

    // One art + one palette, shared by all three balls — the ONLY difference between them is the effect.
    const auto    ball      = beachBallArt();
    const AtlasId ballAtlas = renderer.uploadAtlas(ball.data(), kBall, kBall, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 5> ballPal{{{0, 0, 0},          // index 0 — the hole
                                        {230, 40, 40},      // 1 — red
                                        {40, 200, 60},      // 2 — green
                                        {50, 90, 230},      // 3 — blue
                                        {240, 210, 40}}};   // 4 — yellow
    const PaletteId ballPalId = renderer.uploadPalette(std::span<const Rgba8>(ballPal));

    // The animated ball's eased saturation. `phase` 0 = full color, 1 = gray; `dir` is which way Space last
    // sent it. Advanced on the sim tick so the ease is display-rate-independent.
    float phase = 0.0f;   // 0 -> full color
    int   dir   = +1;     // +1 drains toward gray, -1 floods color back
    constexpr int kDurationTicks = 60;  // ~1s at ~59.7 ticks/s per direction

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Toggle)) {
            dir = -dir;  // reverse the drain each press
            std::printf("[demo] right ball %s\n", dir > 0 ? "draining to grey" : "restoring colour");
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        phase += static_cast<float>(dir) / static_cast<float>(kDurationTicks);
        phase = phase < 0.0f ? 0.0f : (phase > 1.0f ? 1.0f : phase);
    });

    const int ballY = (kViewH - kBall) / 2;
    const std::array<int, 3> ballX{{16, 64, 112}};  // three balls spread across the viewport

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
        auto ballSprite = [&](const char* key, int x) {
            return Sprite{.key = key, .x = x, .y = ballY, .size = AssetDimensions{.width = kBall, .height = kBall},
                          .atlas = ballAtlas, .tile = 0, .palette = ballPalId};
        };

        // LEFT — control: no effect at all, full color.
        sprites.push_back(ballSprite("control", ballX[0]));

        // MIDDLE — a static ColorSaturation at 0: grayscale every frame.
        Sprite grey = ballSprite("static-grey", ballX[1]);
        grey.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 0}};
        sprites.push_back(grey);

        // RIGHT — the eased grade: saturation rides the smoothstepped phase (255 full -> 0 gray), toggled by
        // Space. The value is recomputed and resubmitted each tick; the engine never eases effect params for us.
        const float        sat  = 1.0f - smoothstep(phase);                          // 1 = full color, 0 = gray
        const std::uint8_t satU = static_cast<std::uint8_t>(std::lround(sat * 255.0f));
        Sprite drain = ballSprite("animated", ballX[2]);
        drain.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = satU}};
        sprites.push_back(drain);

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        renderer.renderFrame(frame);
    });

    std::printf("saturation demo — three balls, same art: LEFT full colour (control), MIDDLE greyscale "
                "(ColorSaturation 0), RIGHT an eased drain you toggle. One built-in: "
                "ScreenSpaceEffectKind::ColorSaturation.\n");
    std::printf("[demo] Space = toggle the right ball between colour and grey, Backspace = fullscreen. "
                "Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
