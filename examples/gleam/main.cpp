// Gleam demo — a runnable host that teaches and shows off the built-in GLEAM effect
// (ScreenSpaceEffectKind::Gleam): a luminance-keyed diagonal "sheen sweep", the marquee/logo shine.
//
// WHAT GLEAM IS. A soft diagonal band sweeps across the frame. Where the band covers a pixel, the pixel is
// multiplied UP by its own brightness (plus a small luminance-keyed white lift), so:
//   • BRIGHT content catches the light hard (a specular glint rolling over it),
//   • DARK content barely responds, and true black stays black — no halo, no box, just the shine.
// That "brighter things shine more" behavior is the whole point, so this demo lays out a BRIGHTNESS RAMP —
// six horizontal bands from near-black up to gold — and sweeps the gleam diagonally across all of them. You
// see one diagonal band of light travel down-right; it is invisible on the black band and a strong glint on
// the gold band. That is luminance keying, on screen.
//
// HOW YOU DRIVE IT (the teaching bit). Gleam is a built-in: you never write a shader or register anything —
// you name the kind and set four inline fields with plain designated-init, exactly like Ripple or ColorFill:
//
//     frame.postEffects.push_back(ScreenSpaceEffect{
//         .kind = ScreenSpaceEffectKind::Gleam,
//         .sweep = s,        // WHERE the band is, along its slanted axis (UV units); animate this to sweep
//         .width = 0.12f,    // how THICK the band is (UV units)
//         .gain  = 1.2f,     // how HARD it shines at the crest — 0 is an exact no-op (the default)
//         .slant = 0.35f});  // the diagonal LEAN of the band (axis is uv.x + uv.y*slant)
//
// The band's axis is d = uv.x + uv.y*slant; `sweep` is the band center along d. To make a marquee shine you
// just advance `sweep` over time (below we ramp it across the screen, then park it off-frame to rest — the
// classic "a glint every few seconds" cadence). The engine owns the shader; the game owns the timeline.
// Gleam works at EVERY effect site — here it is a whole-frame postEffect; put the same struct on a
// DrawLayer::effects or inside a Region to confine the shine to one layer or one shape.
//
// DEV KEYS (explore the params live): Up/Down = gain, Left/Right = slant, X = cycle width, Z = pause the
// sweep mid-screen to inspect a frozen band, Backspace = fullscreen. Close to quit.
//
// A dev drives the window.

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
constexpr int kMapW = 20, kMapH = 18;  // 20x18 tiles cover the 160x144 viewport

// The demo's input vocabulary: the four parameter nudges, two toggles, and a dev key.
enum class Action : std::uint8_t {
    GainUp, GainDown, SlantLeft, SlantRight, CycleWidth, PauseSweep, Fullscreen,
};

// The six brightness bands, dark -> bright, that make the luminance keying visible (index 1 of each palette;
// index 0 is unused — the tile is solid index 1). The gold top band is the marquee "sign" that shines most.
constexpr std::array<Rgba8, 6> kBandColour{{
    {10, 10, 14},      // near-black — the shine never lights this
    {48, 48, 62},      // dim
    {96, 96, 116},     // medium
    {150, 150, 170},   // light
    {212, 212, 224},   // bright — a strong glint rolls across
    {255, 214, 120},   // gold — the brightest "logo" band, catches the most light
}};

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Gleam"},
        .window = {.title = "Polyrhythm — gleam demo (luminance-keyed diagonal sheen sweep)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::CycleWidth, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::PauseSweep, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    map.add(presets::directional(Action::GainUp, Action::GainDown, Action::SlantLeft, Action::SlantRight));
    platform.actions(map);

    // Programmatic art — no asset files. ONE solid 8x8 tile (every texel index 1); the band's brightness is
    // its PALETTE, so the same tile drawn under six palettes gives six brightness levels.
    std::array<std::uint8_t, 64> solid{};
    solid.fill(1);
    const AtlasId atlas = renderer.uploadAtlas(solid.data(), 8, 8).atlasId;

    std::array<PaletteId, 6> bandPal{};
    for (std::size_t i = 0; i < bandPal.size(); ++i) {
        const std::array<Rgba8, 2> pal{{{0, 0, 0}, kBandColour[i]}};  // index 0 unused, index 1 = the band color
        bandPal[i] = renderer.uploadPalette(std::span<const Rgba8>(pal));
    }

    // The map: six horizontal bands of three tile-rows each (6*3 = 18), each row's cells drawn under that
    // band's palette. Built once — the art is static; only the gleam's `sweep` animates.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int ty = 0; ty < kMapH; ++ty) {
        const std::size_t band = static_cast<std::size_t>(ty / 3);  // 0..5, top -> bottom
        for (int tx = 0; tx < kMapW; ++tx) {
            cells[static_cast<std::size_t>(ty) * kMapW + tx] =
                TileCell{.atlas = atlas, .tile = 0, .palette = bandPal[band]};
        }
    }

    // Live-tunable gleam parameters (the dev keys nudge these).
    float gain  = 1.2f;    // shine strength at the crest
    float slant = 0.35f;   // diagonal lean of the band
    float width = 0.12f;   // band thickness (UV)
    bool  paused = false;  // Z — park the band mid-screen to inspect a frozen sheen
    constexpr std::array<float, 3> kWidths{{0.06f, 0.12f, 0.22f}};
    std::size_t widthIdx = 1;

    // The sweep timeline: over a ~3.3s cycle, ramp `sweep` from just off the top-left corner to just past the
    // bottom-right (the band's axis d = uv.x + uv.y*slant runs 0..1+slant), then PARK it far off-frame to
    // rest — a glint every few seconds, not a constant strobe. Advanced on the sim tick so the cadence is
    // independent of the display refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::GainUp))     { gain += 0.3f;  std::printf("[dev] gain = %.2f\n", gain); }
        if (in.justPressed(Action::GainDown))   { gain = gain > 0.3f ? gain - 0.3f : 0.0f; std::printf("[dev] gain = %.2f\n", gain); }
        if (in.justPressed(Action::SlantLeft))  { slant -= 0.15f; std::printf("[dev] slant = %.2f\n", slant); }
        if (in.justPressed(Action::SlantRight)) { slant += 0.15f; std::printf("[dev] slant = %.2f\n", slant); }
        if (in.justPressed(Action::CycleWidth)) { widthIdx = (widthIdx + 1) % kWidths.size(); width = kWidths[widthIdx];
                                                  std::printf("[dev] width = %.2f\n", width); }
        if (in.justPressed(Action::PauseSweep)) { paused = !paused; std::printf("[dev] sweep %s\n", paused ? "paused (mid-screen)" : "running"); }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bands{.key = "brightnessBands"};
        bands.z       = 0;
        bands.size    = PixelSize{kViewW, kViewH};
        bands.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(cells)};
        frame.layers.push_back(bands);

        // Compute the sweep position. dMax is the far end of the band axis for the current slant; we travel a
        // little past both ends (± width) so the band fully enters and fully leaves.
        const float dMax = 1.0f + (slant > 0.0f ? slant : 0.0f);
        float       sweep;
        if (paused) {
            sweep = 0.5f * dMax;  // parked over the middle so a frozen band sits across the ramp
        } else {
            constexpr int kPeriod = 200;                     // ~3.3s at ~59.7 ticks/s
            const float   phase   = static_cast<float>(tick % kPeriod) / static_cast<float>(kPeriod);  // 0..1
            constexpr float kActive = 0.5f;                  // half the cycle sweeping, half resting off-frame
            if (phase < kActive) {
                const float t = phase / kActive;             // 0..1 across the active window
                sweep = -width + t * (dMax + 2.0f * width);  // from just off the start to just past the end
            } else {
                sweep = dMax + 10.0f;                        // parked far off-frame — the band contributes nothing
            }
        }

        // The built-in gleam, whole-frame. Same struct shape as every other effect kind; the engine owns the
        // shader. (Put this on a DrawLayer::effects to shine one layer, or in a Region to confine it to a
        // shape — a marquee sign.) gain 0 would be an exact no-op.
        frame.postEffects.clear();
        frame.postEffects.push_back(ScreenSpaceEffect{
            .kind = ScreenSpaceEffectKind::Gleam, .sweep = sweep, .width = width, .gain = gain, .slant = slant});

        renderer.renderFrame(frame);
    });

    std::printf("gleam demo — a diagonal SHEEN sweeps down-right across six brightness bands (near-black -> "
                "gold). Watch it glint hard on the bright/gold bands and not at all on the dark ones: the "
                "shine is LUMINANCE-KEYED. One built-in: ScreenSpaceEffectKind::Gleam.\n");
    std::printf("[dev] Up/Down = gain, Left/Right = slant, X = width, Z = pause sweep, Backspace = fullscreen. "
                "Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
