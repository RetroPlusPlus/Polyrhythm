// Sprite transform showcase demo — the capstone of the transform era:
//
//   • A big 16×16 "F" GLYPH SPRITE (z=10) that spins about ITS OWN center under Sprite::transform — a
//     per-sprite projective transform, the sprite analog of D.1's per-layer Mode-7 floor. The F is
//     asymmetric in BOTH axes, so rotation orientation, flip, and perspective all read clearly. Toggle
//     a slow scale pulse (Left), a perspective foreshorten (Up), a horizontal flip (Z), and a vertical
//     flip (Down) to prove the flips compose with rotation (a flip mirrors the TEXTURE within the
//     sprite; the transform rotates the QUAD — independent operations).
//   • A RIDE LAYER (z=20) of three small F glyphs whose DrawLayer::transform slowly rotates the WHOLE
//     layer about the viewport center — the glyphs orbit rigidly together, proving the per-layer
//     transform reaches sprites (each sprite carries identity; the layer moves them all).
//   • A static checkerboard BACKGROUND (z=0, tile path) so the sprite motion reads against a grid.
//
// Rotation/zoom are slow and same-direction — no strobing — and nothing auto-launches. Run on a dev
// machine and confirm: the central F spins about its own center (and foreshortens / pulses / flips to a
// backward-F per the toggles), while the three small F's orbit together as one rigid layer.
//
// Like the other example hosts this instantiates SdlPlatform + Renderer in a real run, so it keeps the
// live SDL_GPU sprite transform path compiling + linking on every CI platform even though CI never
// opens the window.

#include <array>
#include <cmath>
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
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kBgMapW = 20, kBgMapH = 18;   // 20×18 8px tiles cover 160×144 exactly

// The demo's input vocabulary: the spinner toggles + the window/render dev controls.
enum class Action : std::uint8_t {
    Perspective, ScalePulse, FlipHorizontal, FlipVertical, Fullscreen, SamplingToggle, WindowScale,
};

// Build a 16×16 indexed "F" glyph — asymmetric in BOTH axes so rotation AND flip read clearly (the
// classic transform-test glyph). index 0 = transparent (OBJ hole), 1 = the vertical spine, 2 = the
// two arms. A horizontal flip turns it into a backward F; any rotation is unambiguous.
std::array<std::uint8_t, 16 * 16> makeGlyphAtlas() {
    std::array<std::uint8_t, 16 * 16> px{};  // zero = transparent
    auto set = [&](int x, int y, std::uint8_t v) {
        px[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] = v;
    };
    for (int y = 2; y <= 13; ++y)                       // vertical spine: cols 3..5, rows 2..13
        for (int x = 3; x <= 5; ++x) set(x, y, 1);
    for (int y = 2; y <= 4; ++y)                        // top arm: rows 2..4, cols 3..12
        for (int x = 3; x <= 12; ++x) set(x, y, 2);
    for (int y = 7; y <= 9; ++y)                        // middle arm: rows 7..9, cols 3..10
        for (int x = 3; x <= 10; ++x) set(x, y, 2);
    return px;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Sprite Transform Demo"},
        .window = {.title = "Polyrhythm — sprite transform showcase: spinning F + orbiting ride layer"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Perspective,    {SDL_SCANCODE_UP, SDL_SCANCODE_W, PadButton::DpadUp}},
        {Action::ScalePulse,     {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::FlipHorizontal, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::FlipVertical,   {SDL_SCANCODE_DOWN, SDL_SCANCODE_S, PadButton::DpadDown}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::SamplingToggle, {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::WindowScale,    {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    int windowScale = config.enhancements.windowScale;

    // ── Background tile atlas: index 1 = fill (the checkerboard selects a palette per cell). ──
    constexpr int kBgAtlasW = 8, kBgAtlasH = 8;
    std::array<std::uint8_t, kBgAtlasW * kBgAtlasH> bgPx{};
    bgPx.fill(1);
    const AtlasId bgAtlas = renderer.uploadAtlas(bgPx.data(), kBgAtlasW, kBgAtlasH).atlasId;

    // ── Sprite atlas: the 16×16 F glyph (atlas is 2×2 cells; sprite tile 0 reads the whole thing). ──
    const std::array<std::uint8_t, 16 * 16> glyphPx = makeGlyphAtlas();
    const AtlasId glyphAtlas = renderer.uploadAtlas(glyphPx.data(), 16, 16, TransparentIndices::GameBoy).atlasId;

    // ── Palettes. ──────────────────────────────────────────────────────────────────────────────
    const std::array<Rgba8, 2> palBgA{{{0, 0, 0}, {54, 58, 74}}};      // dark slate
    const std::array<Rgba8, 2> palBgB{{{0, 0, 0}, {38, 41, 53}}};      // darker slate (checker)
    const std::array<Rgba8, 3> palGlyph{{{0, 0, 0}, {230, 206, 92}, {236, 120, 72}}};  // spine gold, arms orange
    const PaletteId bgA    = renderer.uploadPalette(std::span<const Rgba8>(palBgA));
    const PaletteId bgB    = renderer.uploadPalette(std::span<const Rgba8>(palBgB));
    const PaletteId glyphP = renderer.uploadPalette(std::span<const Rgba8>(palGlyph));
    const std::array<PaletteId, 2> bgPals{bgA, bgB};  // the checkerboard picks one per cell, directly

    // ── Background checkerboard map (kept alive for the program's duration). ──────────────────────
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kBgMapW) * kBgMapH);
    for (int y = 0; y < kBgMapH; ++y) {
        for (int x = 0; x < kBgMapW; ++x) {
            bgCells[static_cast<std::size_t>(y) * kBgMapW + x] =
                TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPals[static_cast<std::size_t>((x + y) & 1)]};
        }
    }

    // The spinner: one 16×16 F centered in the viewport. Its own transform spins it about (8,8).
    Sprite spinner{.key = "spinner"};
    spinner.size    = AssetDimensions{16, 16};
    spinner.tile    = 0;
    spinner.atlas   = glyphAtlas;
    spinner.palette = glyphP;
    spinner.x       = kViewW / 2 - 8;   // center the 16×16 sprite
    spinner.y       = kViewH / 2 - 8;

    // The ride layer: three F glyphs in a row near the top; the LAYER transform orbits them.
    constexpr std::array<const char*, 3> riderKeys{"rider0", "rider1", "rider2"};
    std::array<Sprite, 3> riders{{{.key = "rider0"}, {.key = "rider1"}, {.key = "rider2"}}};
    for (int i = 0; i < 3; ++i) {
        riders[static_cast<std::size_t>(i)] = Sprite{.key = riderKeys[static_cast<std::size_t>(i)],
                                                     .x = 40 + i * 40, .y = 24, .size = AssetDimensions{16, 16},
                                                     .atlas = glyphAtlas, .tile = 0, .palette = glyphP};
    }

    bool perspective = false;  // Up:   foreshorten the spinner
    bool scalePulse  = true;   // Left: a slow scale pulse on the spinner
    bool flipX       = false;  // Z:    flipX on the spinner (composes with the rotation)
    bool flipY       = false;  // Down: flipY on the spinner (composes with the rotation)

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::Perspective)) {
            perspective = !perspective;
            std::printf("[dev] spinner perspective: %s\n", perspective ? "on" : "off");
        }
        if (in.justPressed(Action::ScalePulse)) {
            scalePulse = !scalePulse;
            std::printf("[dev] spinner scale pulse: %s\n", scalePulse ? "on" : "off");
        }
        if (in.justPressed(Action::FlipHorizontal)) {
            flipX = !flipX;
            std::printf("[dev] spinner flipX (F ↔ backwards-F, composes with rotation): %s\n",
                        flipX ? "on" : "off");
        }
        if (in.justPressed(Action::FlipVertical)) {
            flipY = !flipY;
            std::printf("[dev] spinner flipY (F ↔ upside-down F, composes with rotation): %s\n",
                        flipY ? "on" : "off");
        }
        if (in.justPressed(Action::Fullscreen)) {
            platform.window().fullscreen(!platform.window().fullscreen());
        }
        if (in.justPressed(Action::SamplingToggle)) {
            const bool toBilinear = renderer.samplingMode() == SamplingMode::Nearest;
            renderer.samplingMode(toBilinear ? SamplingMode::Bilinear : SamplingMode::Nearest);
            std::printf("[dev] sampling: %s\n", toBilinear ? "bilinear" : "nearest");
        }
        if (in.justPressed(Action::WindowScale)) {
            windowScale = (windowScale >= 8) ? 1 : windowScale + 1;
            const PixelSize vp{kViewW, kViewH};
            const int eff = fitWindowScale(vp, platform.usableDisplaySize(), windowScale);
            if (!platform.window().fullscreen()) platform.window().size(PixelSize{vp.width * eff, vp.height * eff});
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();

        // z=0: static checkerboard background (tile path), so the sprite motion reads against a grid.
        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles  = kBgMapW,
                                 .heightInTiles = kBgMapH,
                                 .cells         = std::span<const TileCell>(bgCells)};
        frame.layers.push_back(bg);

        // z=10: the spinner. Sprite::transform spins about the sprite's own center (8,8), optionally
        // pulsing in scale and foreshortening under perspective — all in sprite-local space.
        const float spin  = static_cast<float>(tick) * 0.4f;   // ~24°/s — slow, no strobe
        const float pulse = scalePulse ? 1.0f + 0.3f * std::sin(static_cast<float>(tick) * 0.015f) : 1.0f;
        Transform spinnerT = Transform::rotation(spin, 8.0f, 8.0f);
        if (scalePulse) spinnerT = Transform::scale(pulse, pulse, 8.0f, 8.0f).then(spinnerT);
        if (perspective) spinnerT = spinnerT.then(Transform::perspective(0.012f, 0.0f));

        Sprite spin1 = spinner;
        spin1.transform = spinnerT;
        spin1.flipX     = flipX;
        spin1.flipY     = flipY;
        const std::array<Sprite, 1> spinArr{spin1};

        DrawLayer spinLayer{.key = "spinner"};
        spinLayer.z       = 10;
        spinLayer.size    = PixelSize{kViewW, kViewH};
        spinLayer.content = SpriteContent{.sprites = std::span<const Sprite>(spinArr)};
        frame.layers.push_back(spinLayer);

        // z=20: the ride layer — three identity sprites; the LAYER transform orbits them all rigidly
        // about the viewport center (proving DrawLayer::transform reaches the sprite path).
        DrawLayer rideLayer{.key = "ride"};
        rideLayer.z         = 20;
        rideLayer.size      = PixelSize{kViewW, kViewH};
        rideLayer.content   = SpriteContent{.sprites = std::span<const Sprite>(riders)};
        rideLayer.transform = Transform::rotation(static_cast<float>(tick) * 0.2f,  // slow orbit
                                                  kViewW / 2.0f, kViewH / 2.0f);
        frame.layers.push_back(rideLayer);

        renderer.renderFrame(frame);
    });

    std::printf("Sprite transform showcase — a 16×16 F spins about its own centre while "
                "three F's orbit together as one rigid ride layer.\n");
    std::printf("[dev] Up = spinner perspective, Left = scale pulse, Z = flipX (F ↔ backwards-F), "
                "Down = flipY (F ↔ upside-down F), Backspace = fullscreen, Return = sampling, "
                "X = window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
