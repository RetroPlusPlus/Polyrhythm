// Transform showcase demo — every tile-path transform feature in one scene:
//
//   • A Mode-7-style checkerboard FLOOR (z=10) that rotates (yaw) and recedes under a perspective
//     foreshortening — the spinning Mario-Kart-style ground, realized as ONE projective transform
//     evaluated per-pixel in the tile fragment (no per-scanline anything, no hardware idiom).
//   • The FOOTPRINT EDGE POLICY shown live on the rotating floor: as the square rotates into a diamond
//     the exposed corners are Blank (transparent → the sky backdrop shows through) or Stretch
//     (clamp-to-edge → the border smears). Toggle with Z.
//   • Per-layer SCALE (a slow zoom pulse, toggle Left) and PERSPECTIVE (toggle Up) — composed with the
//     rotation, proving scale + rotate + perspective stack in one Transform.
//   • A static SKY backdrop (z=0) the floor's blank corners reveal.
//   • A translucent WAVY HAZE band (z=20) — per-layer alpha + a RowDisplacement effect + index-hole
//     transparency — showing screen-space effects + alpha compose with a transformed layer below.
//   • Multiple PALETTES (the checkerboard alternates two palettes per cell).
//   • A frame-level day/night tint — a Multiply ColorFill region (toggle Down).
//   • The floor's TILEMAP WRAP MODE (toggle Right): Repeat (the map tiles infinitely as the floor
//     scrolls forward) → Clamp (the edge row smears) → Blank (a FINITE floor that ends at the map
//     edge as it scrolls off, revealing the sky — the mode a finite overworld map needs).
//
// Rotation/zoom/tint are all slow and same-direction — no strobing — and nothing auto-launches.
// Run on a dev machine and confirm: the floor spins + recedes, its corners reveal the sky (Blank) or
// smear (Stretch), the haze band waves over it, and the day/night tint shifts the whole frame.
//
// (A later demo adds a rotating SPRITE into this same scene → the full transform capstone.)
//
// Like the other example hosts, this instantiates SdlPlatform + Renderer in a real run, so it keeps
// the live SDL_GPU transform path compiling + linking on every CI platform even though CI never opens
// the window.

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
constexpr int kMapW  = 20, kMapH = 18;   // 20×18 tiles cover the 160×144 footprint exactly (tiles wrap)

// Atlas tiles (3 tiles in a row, 24×8 indices): 0 = transparent hole, 1 = grid cell (lined border over
// a fill), 2 = solid fill (sky / haze).
enum Tile : std::uint16_t { TileHole = 0, TileGrid = 1, TileSolid = 2 };

// The demo's input vocabulary: the floor toggles + the window/render dev controls.
enum class Action : std::uint8_t {
    Perspective, ZoomPulse, EdgeToggle, DayNight, WrapCycle, Fullscreen, SamplingToggle, WindowScale,
};

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Transform Demo"},
        .window = {.title = "Polyrhythm — transform showcase: Mode-7 floor + edge policy + effects"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::EdgeToggle,     {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::Fullscreen,     {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
        {Action::SamplingToggle, {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Action::WindowScale,    {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    map.add(presets::directional(Action::Perspective, Action::DayNight,
                                 Action::ZoomPulse, Action::WrapCycle));  // Up / Down / Left / Right
    platform.actions(map);

    int windowScale = config.enhancements.windowScale;

    // ── One indexed atlas: hole · grid cell · solid. Index 1 = fill, 2 = grid line, 0 = transparent. ──
    constexpr int kAtlasTiles = 3;
    constexpr int kAtlasW     = kAtlasTiles * 8;  // 24
    constexpr int kAtlasH     = 8;
    std::array<std::uint8_t, static_cast<std::size_t>(kAtlasW) * kAtlasH> atlasPx{};
    for (int y = 0; y < kAtlasH; ++y) {
        for (int t = 0; t < kAtlasTiles; ++t) {
            for (int x = 0; x < 8; ++x) {
                std::uint8_t idx = 0;
                switch (t) {
                    case TileHole:  idx = 0; break;                                  // transparent marker
                    case TileGrid:  idx = 1; break;                                  // solid (grid lines removed: 1px lines aliased badly under perspective minification)
                    case TileSolid: idx = 1; break;                                  // solid fill
                }
                atlasPx[static_cast<std::size_t>(y) * kAtlasW + static_cast<std::size_t>(t) * 8 + x] = idx;
            }
        }
    }
    const AtlasId opaqueAtlas = renderer.uploadAtlas(atlasPx.data(), kAtlasW, kAtlasH).atlasId;                  // floor + sky
    const AtlasId holeAtlas   = renderer.uploadAtlas(atlasPx.data(), kAtlasW, kAtlasH, TransparentIndices::of({0})).atlasId;  // haze

    // ── Palettes: two floor palettes (checkerboard), a sky, a haze. ────────────────────────────────
    const std::array<Rgba8, 3> palFloorA{{{0, 0, 0}, {206, 206, 216}, {44, 46, 58}}};  // light cells, dark lines
    const std::array<Rgba8, 3> palFloorB{{{0, 0, 0}, {206, 78, 78}, {44, 46, 58}}};    // crimson cells, dark lines
    const std::array<Rgba8, 3> palSky{{{0, 0, 0}, {104, 166, 230}, {0, 0, 0}}};        // sky blue
    const std::array<Rgba8, 3> palHaze{{{0, 0, 0}, {206, 236, 246}, {0, 0, 0}}};       // pale cyan
    const PaletteId floorA = renderer.uploadPalette(std::span<const Rgba8>(palFloorA));
    const PaletteId floorB = renderer.uploadPalette(std::span<const Rgba8>(palFloorB));  // checkerboard alternates A/B per cell
    const PaletteId skyP   = renderer.uploadPalette(std::span<const Rgba8>(palSky));
    const PaletteId hazeP  = renderer.uploadPalette(std::span<const Rgba8>(palHaze));

    // ── Tilemaps (kept alive for the program's duration). ──────────────────────────────────────────
    std::vector<TileCell> floorCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> skyCells(static_cast<std::size_t>(kMapW) * kMapH);
    std::vector<TileCell> hazeCells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const auto i = static_cast<std::size_t>(y) * kMapW + x;
            floorCells[i] = TileCell{.atlas = opaqueAtlas, .tile = TileGrid,
                                     .palette = ((x + y) & 1) ? floorB : floorA};  // 8px checker
            skyCells[i]   = TileCell{.atlas = opaqueAtlas, .tile = TileSolid, .palette = skyP};
            hazeCells[i]  = TileCell{.atlas = holeAtlas,
                                     .tile = (y >= 6 && y <= 8) ? TileSolid : TileHole, .palette = hazeP};      // a band
        }
    }

    bool     perspective = true;   // Up:    Mode-7 perspective recede vs a flat spin
    bool     zoomPulse   = true;   // Left:  a slow scale pulse (shows scale composing with rotation)
    bool     stretchEdge = false;  // Z:     footprint edge — Blank (reveal sky) vs Stretch (clamp/smear)
    bool     dayNight    = false;  // Down:  frame-level day/night tint (Multiply ColorFill region)
    TileWrap floorWrap   = TileWrap::Repeat;  // Right: tilemap wrap — Repeat (infinite) / Clamp / Blank (finite)

    // Advance animation on the sim tick below, not in the render callback, so motion speed is
    // independent of the display's refresh rate.
    int tick = 0;
    loop.simTick([&](const InputState& in) {
        ++tick;
        if (in.justPressed(Action::Perspective)) {
            perspective = !perspective;
            std::printf("[dev] perspective (Mode-7 recede): %s\n", perspective ? "on" : "off");
        }
        if (in.justPressed(Action::ZoomPulse)) {
            zoomPulse = !zoomPulse;
            std::printf("[dev] zoom pulse (scale): %s\n", zoomPulse ? "on" : "off");
        }
        if (in.justPressed(Action::EdgeToggle)) {
            stretchEdge = !stretchEdge;
            std::printf("[dev] footprint edge: %s\n", stretchEdge ? "Stretch (clamp)" : "Blank (reveal sky)");
        }
        if (in.justPressed(Action::DayNight)) {
            dayNight = !dayNight;
            std::printf("[dev] day/night tint: %s\n", dayNight ? "on" : "off");
        }
        if (in.justPressed(Action::WrapCycle)) {
            floorWrap = (floorWrap == TileWrap::Repeat) ? TileWrap::Clamp
                      : (floorWrap == TileWrap::Clamp)  ? TileWrap::Blank
                                                        : TileWrap::Repeat;
            const char* name = (floorWrap == TileWrap::Repeat) ? "Repeat (infinite tiling)"
                             : (floorWrap == TileWrap::Clamp)  ? "Clamp (edge smear)"
                                                               : "Blank (finite floor — ends at the map edge)";
            std::printf("[dev] floor tilemap wrap: %s\n", name);
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
        frame.regions.clear();

        // z=0: sky backdrop — full viewport, static. The floor's Blank corners reveal this.
        DrawLayer sky{.key = "sky"};
        sky.z       = 0;
        sky.size    = PixelSize{kViewW, kViewH};
        sky.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                  .cells = std::span<const TileCell>(skyCells)};
        frame.layers.push_back(sky);

        // z=10: the Mode-7-style floor. Build the transform: a slow scale pulse, THEN a slow yaw spin
        // about the viewport center, THEN (optionally) a perspective foreshortening so the far side
        // recedes — one Transform the fragment inverts + perspective-divides per pixel. The footprint
        // edge policy fills the rotated diamond's corners. (Perspective strength is dev-tunable; the
        // floor scrolls so the ground appears to drive forward.)
        const float angle = static_cast<float>(tick) * 0.25f;                  // ~15°/s — slow, no strobe
        const float pulse = 1.0f + 0.25f * std::sin(static_cast<float>(tick) * 0.012f);
        Transform floorT = Transform::rotation(angle, kViewW / 2.0f, kViewH / 2.0f);
        if (zoomPulse) {
            floorT = Transform::scale(pulse, pulse, kViewW / 2.0f, kViewH / 2.0f).then(floorT);
        }
        if (perspective) {
            // Vertical foreshortening toward a horizon — the receding-ground look. The sign/strength
            // here place the horizon; tune on a dev machine for the exact framing.
            floorT = floorT.then(Transform::perspective(0.0f, -0.0045f));
        }

        DrawLayer floor{.key = "mode7Floor"};
        floor.z             = 10;
        floor.size          = PixelSize{kViewW, kViewH};
        floor.scroll        = LayerScroll{0, tick / 2};   // drive forward gently
        floor.content       = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                          .cells = std::span<const TileCell>(floorCells), .wrap = floorWrap};
        floor.transform     = floorT;
        floor.transformEdge = stretchEdge ? DisplacementEdge::Stretch : DisplacementEdge::Blank;
        frame.layers.push_back(floor);

        // z=20: a translucent wavy haze band — per-layer alpha + a Layer-scope RowDisplacement + index-
        // hole transparency, composited over the transformed floor (effects/alpha compose with a
        // transformed layer below in the same frame).
        DrawLayer haze{.key = "haze"};
        haze.z       = 20;
        haze.size    = PixelSize{kViewW, kViewH};
        haze.alpha   = 0.55f;
        haze.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                   .cells = std::span<const TileCell>(hazeCells)};
        haze.effects = {ScreenSpaceEffect{
            .kind      = ScreenSpaceEffectKind::RowDisplacement,
            .amplitude = 3.0f,
            .frequency = 2.0f,
            .phase     = static_cast<float>(tick) * 0.006f,   // slow drift
            .axis      = Axis::Horizontal,
            .edge      = DisplacementEdge::Blank,
            .scope     = ScreenSpaceEffectScope::Layer}};
        frame.layers.push_back(haze);

        // Frame-level day/night (toggle): a slow warm→cool oscillation over the whole composited frame,
        // expressed as an ordinary effect — a Multiply-blended ColorFill region covering the viewport. The
        // fill color is the per-channel multiplier; Multiply darkens/tints the scene. No bespoke frame
        // color member.
        if (dayNight) {
            const float t   = 0.5f + 0.5f * std::sin(static_cast<float>(tick) * 0.004f);  // 0..1, slow
            const auto  u8  = [](float v) { return static_cast<std::uint8_t>(v * 255.0f); };
            const Rgba8 tint{u8(0.65f + 0.35f * t), u8(0.70f + 0.30f * t), u8(0.80f + 0.20f * (1.0f - t)), 255};
            frame.regions.push_back(Region{
                .key     = "dayNight",
                .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = tint}},
                .blend   = BlendMode::Multiply});
        }

        renderer.renderFrame(frame);
    });

    std::printf("Transform showcase — a Mode-7-style checkerboard floor spins + recedes; its "
                "rotated corners reveal the sky (Blank) or smear (Stretch); a wavy translucent haze "
                "rides over it.\n");
    std::printf("[dev] Up = perspective, Left = zoom pulse, Z = edge Blank/Stretch, Right = floor wrap "
                "Repeat/Clamp/Blank, Down = day/night, Backspace = fullscreen, Return = sampling, "
                "X = window scale.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
