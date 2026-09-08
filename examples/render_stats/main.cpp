// Render-stats demo — a 1080p load bench behind a hi-fi deck.
//
// WHAT THIS IS FOR. `Renderer::renderStats()` reports what the renderer did and what it cost:
// cumulative upload / compose / present counters, each paired with its skip counter, plus the last
// frame's cost split by phase. This demo puts that surface on screen and gives you controls that each
// load a different part of the renderer, so a number can be tied to a cause rather than guessed at.
//
// THE DECK. The panel along the bottom is the control surface. Three rotary knobs carry the ranged
// settings and their pointers ARE the value — the sprite's own transform turns them. The lamps beside
// them are the toggles, lit or unlit by palette. Everything is reachable three ways: the mouse (drag a
// knob, click a lamp), the keyboard (arrows), and a gamepad (d-pad). The rightmost lamp shows or hides
// the text readout, so the deck alone can drive the whole demo.
//
// THE READOUT. Top-left, when its lamp is lit — the stats block, then the same settings as a text
// menu with the cursor on whichever control is selected. The menu and the deck are two views of one
// state: turn a knob and the menu row moves, arrow onto a row and the knob turns. FPS is PRESENTED frames — the ones a viewer actually
// saw — and CALLS is how often the render callback ran. On Metal the swapchain is acquired
// non-blocking, so when the GPU falls behind the callback keeps firing at full rate while presents are
// quietly skipped; CALLS far above FPS is that, and MISSED counts it. Below that is where the frame's
// time went in microseconds, then the work counters as ISSUED / SKIPPED.
//
// WHAT EACH CONTROL LOADS:
//
//   SPRITES        instanced sprite draws and their per-frame record upload
//   LAYERS         extra full-viewport tile layers — an upload and a composite each
//   LENSES         Below-scope lenses, each reading the scene through its own emission field
//   BLOOM          a region Bloom per sprite — one field each, packed and blurred by page
//   ROTATE         per-sprite transforms, which puts coverage on the analytic path
//   RUNS           how many contiguous same-blend runs the sprite layer splits into
//   RIPPLE         a screen-space effect over the scene layer
//   INTERP         easing every sprite between ticks
//   OUTGRID        output-grid (smooth) evaluation instead of viewport-grid (crisp)
//   MONITOR        the text readout — the stats block and the settings menu beneath it
//
// Turning one on and watching which phase and which counter move is the whole point. Sprites move the
// sprite upload counter and COMPOSE; a blend split adds passes without adding sprites; a still screen
// drives COMPOSE to zero and the compose SKIP counter to the refresh rate.
//
// Art is generated — see assets/gen_render_stats_assets.py. Motion advances on the sim tick, so the
// load is the same on any display.
//
// CAPTURE MODE. `retropp-render_stats-demo <seconds> [out.csv] [sprites]` runs the bench for a fixed
// time and writes one CSV row per frame — the same phase split and issued/skipped counters the readout
// shows, recorded per frame rather than watched on screen. Rows buffer in memory and the file is written
// once at exit, because a write per frame perturbs what is being measured. The sprite count is pinned
// from the command line and recorded in every row, so two captures taken for comparison run identical
// work and can be shown to have done so; the rest of the deck stays live. With no arguments the demo
// runs interactively and records nothing.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 1920, kViewH = 1080;
constexpr int kTilePx = 8;

// One frame's sample in capture mode. Plain values, so the run reserves once and never reallocates
// while it is measuring.
struct CaptureRow {
    std::uint64_t frame          = 0;
    std::uint64_t tNs            = 0;
    std::uint64_t ticks          = 0;
    std::uint64_t tickCount      = 0;
    double        acquireMs      = 0.0;
    double        interpMs       = 0.0;
    double        composeMs      = 0.0;
    double        presentMs      = 0.0;
    int           presented      = 0;
    int           composeSkipped = 0;
    std::uint64_t presentPasses  = 0;
    std::uint64_t presentSkips   = 0;
    std::uint64_t composePasses  = 0;
    std::uint64_t composeSkips   = 0;
    std::uint64_t tilemapUploads = 0;
    std::uint64_t spriteUploads  = 0;
    std::uint64_t scene          = 0;
    double        simMs          = 0.0;
    int           sprites        = 0;   // the dominant load knob, recorded so two runs are comparable
};

// Write the buffered rows once. The column order matches the frame-pacing captures the analysis
// scripts read, so a run compares directly against an earlier one; append columns, never reorder.
void writeCapture(const std::string& path, const std::vector<CaptureRow>& rows) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "render_stats: cannot open %s for writing\n", path.c_str());
        return;
    }
    std::fprintf(out,
                 "frame,t_ns,ticks,tick_count,acquire_ms,interp_ms,compose_ms,present_ms,presented,"
                 "compose_skipped,present_passes,present_skips,compose_passes,compose_skips,"
                 "tilemap_uploads,sprite_uploads,scene,sim_ms,sprites\n");
    for (const CaptureRow& r : rows) {
        std::fprintf(out,
                     "%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,"
                     "%llu,%.6f,%d\n",
                     static_cast<unsigned long long>(r.frame),
                     static_cast<unsigned long long>(r.tNs),
                     static_cast<unsigned long long>(r.ticks),
                     static_cast<unsigned long long>(r.tickCount),
                     r.acquireMs, r.interpMs, r.composeMs, r.presentMs,
                     r.presented, r.composeSkipped,
                     static_cast<unsigned long long>(r.presentPasses),
                     static_cast<unsigned long long>(r.presentSkips),
                     static_cast<unsigned long long>(r.composePasses),
                     static_cast<unsigned long long>(r.composeSkips),
                     static_cast<unsigned long long>(r.tilemapUploads),
                     static_cast<unsigned long long>(r.spriteUploads),
                     static_cast<unsigned long long>(r.scene),
                     r.simMs, r.sprites);
    }
    std::fclose(out);
    std::fprintf(stderr, "render_stats: wrote %zu rows to %s\n", rows.size(), path.c_str());
}

// Authored cell sizes, and the 8px-tile stride each sheet stamps at (sheet width / 8).
constexpr int kGlyphPx = 16, kGlyphStride = 128 / kTilePx;   // font.png is 128 wide
constexpr int kFieldPx = 32, kFieldStride = 256 / kTilePx;   // the 8-cell 32px sheets are 256 wide
constexpr int kKnobPx = 96, kLampPx = 48;

// The deck.
constexpr int kDeckTop = 856;                       // the face begins here and runs to the bottom
constexpr int kControlY = 946;                      // every control's centre line
constexpr int kLabelY = 1012;
constexpr int kControlX0 = 112, kControlDx = 178;
constexpr float kKnobSweep = 135.0f;                // the pointer's travel either side of centre

constexpr int kMaxSprites = 4000, kMaxLenses = 24, kMaxTileLayers = 4, kMaxRuns = 24;

enum class Action : std::uint8_t { Up, Down, Less, More, Fullscreen };

enum class Ctl : std::size_t {
    Sprites, TileLayers, BelowLenses, BlendRuns, RegionBloom, Rotation, FrameRipple,
    Interpolation, OutputGrid, Monitor, Count
};
constexpr auto kCtlCount = static_cast<std::size_t>(Ctl::Count);

// One control on the face. A rotary carries a range; the rest are lamps.
struct ControlSpec {
    bool        rotary;
    const char* label;
};
constexpr std::array<ControlSpec, kCtlCount> kDeck{{
    {true, "SPRITES"}, {true, "LAYERS"}, {true, "LENSES"}, {true, "RUNS"},
    {false, "BLOOM"}, {false, "ROTATE"}, {false, "RIPPLE"},
    {false, "INTERP"}, {false, "OUTGRID"}, {false, "MONITOR"},
}};

// Everything the deck controls.
struct Load {
    int  sprites       = 400;
    int  tileLayers    = 1;
    int  belowLenses   = 0;
    int  blendRuns     = 0;      // 0 = one run; higher splits the layer into that many
    bool regionBloom   = false;
    bool rotation      = false;
    bool frameRipple   = false;
    bool interpolation = true;
    bool outputGrid    = false;
    bool monitor       = true;
};

[[nodiscard]] Point controlCenter(std::size_t i) {
    return Point{static_cast<float>(kControlX0 + static_cast<int>(i) * kControlDx),
                 static_cast<float>(kControlY)};
}

// A rotary's value as a fraction of its range — what the pointer angle is drawn from.
[[nodiscard]] float rotaryFraction(const Load& l, Ctl c) {
    switch (c) {
        case Ctl::Sprites:     return static_cast<float>(l.sprites) / kMaxSprites;
        case Ctl::TileLayers:  return static_cast<float>(l.tileLayers) / kMaxTileLayers;
        case Ctl::BelowLenses: return static_cast<float>(l.belowLenses) / kMaxLenses;
        case Ctl::BlendRuns:   return static_cast<float>(l.blendRuns) / kMaxRuns;
        default:               return 0.0f;
    }
}

[[nodiscard]] bool lampLit(const Load& l, Ctl c) {
    switch (c) {
        case Ctl::RegionBloom:   return l.regionBloom;
        case Ctl::Rotation:      return l.rotation;
        case Ctl::FrameRipple:   return l.frameRipple;
        case Ctl::Interpolation: return l.interpolation;
        case Ctl::OutputGrid:    return l.outputGrid;
        case Ctl::Monitor:       return l.monitor;
        default:                 return false;
    }
}

[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    return 36;
}

// `n` right-aligned in `width` columns, so a rolling digit never shifts its row.
[[nodiscard]] std::string pad(std::uint64_t n, int width) {
    std::string s = std::to_string(n);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), ' ');
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    // Capture mode: `render_stats <seconds> [out.csv]` runs the bench for a fixed time, records one row
    // per frame in memory, writes the file once at exit and closes itself. With no arguments the demo is
    // exactly what it is interactively, and records nothing.
    const double            capSeconds = (argc > 1) ? std::strtod(argv[1], nullptr) : 0.0;
    const std::string       capPath    = (argc > 2) ? argv[2] : "render_stats_capture.csv";
    const int               capSprites = (argc > 3) ? static_cast<int>(std::strtol(argv[3], nullptr, 10))
                                                    : -1;
    const bool              capturing  = capSeconds > 0.0;
    std::vector<CaptureRow> capRows;
    if (capturing) capRows.reserve(static_cast<std::size_t>(capSeconds * 70.0) + 64);
    const auto    capStart     = std::chrono::steady_clock::now();
    std::uint64_t capLastTicks = 0;
    double        capSimMs     = 0.0;

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "RenderStats"},
        .window   = {.title = "Polyrhythm — render stats (1080p load bench)"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Up, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::Down, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Less, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::More, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // ── Art: generated sheets, coloured by palette images ────────────────────────────────────
    const AtlasManifest font =
        renderer.loadAtlas("examples/render_stats/assets/art/font.png",
                           AssetDimensions{kGlyphPx, kGlyphPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 64, TransparentIndices::None, 0,
                           AssetPolicy::Embed);
    const AtlasManifest knobs =
        renderer.loadAtlas("examples/render_stats/assets/art/knobs.png",
                           AssetDimensions{kKnobPx, kKnobPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 1, TransparentIndices::None, 0,
                           AssetPolicy::Embed);
    const AtlasManifest lamps =
        renderer.loadAtlas("examples/render_stats/assets/art/lamps.png",
                           AssetDimensions{kLampPx, kLampPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 1, TransparentIndices::None, 0,
                           AssetPolicy::Embed);
    const AtlasManifest deck =
        renderer.loadAtlas("examples/render_stats/assets/art/deck.png",
                           AssetDimensions{kFieldPx, kFieldPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 6, TransparentIndices::None, 0,
                           AssetPolicy::Embed);
    const AtlasManifest backdrop =
        renderer.loadAtlas("examples/render_stats/assets/art/backdrop.png",
                           AssetDimensions{kFieldPx, kFieldPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 8, TransparentIndices::None, 0,
                           AssetPolicy::Embed);
    const AtlasManifest blobs =
        renderer.loadAtlas("examples/render_stats/assets/art/blobs.png",
                           AssetDimensions{kFieldPx, kFieldPx}, ContentKind::Tileset,
                           ReadOrder::LeftRightThenDown, 8, TransparentIndices::None, 0,
                           AssetPolicy::Embed);

    const PaletteId palFont =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/font.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palFontPick =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/font_pick.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palMono =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/mono.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palMonoPick =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/mono_pick.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palKnob =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/knob.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palLampOff =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/lamp_off.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palLampOn =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/lamp_on.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palDeck =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/deck.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palDeckRail =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/deck_rail.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palDeckShade =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/deck_shade.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palBackdrop =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/backdrop.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    const PaletteId palBlob =
        renderer.loadPaletteImage("examples/render_stats/assets/palettes/blob.png",
                                  ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    // ── Stable keys, minted once ─────────────────────────────────────────────────────────────
    std::vector<std::string> blobKeys, lensKeys, layerKeys, ctlKeys;
    for (int i = 0; i < kMaxSprites; ++i) blobKeys.push_back("blob" + std::to_string(i));
    for (int i = 0; i < kMaxLenses; ++i)  lensKeys.push_back("lens" + std::to_string(i));
    for (int i = 0; i < kMaxTileLayers; ++i) layerKeys.push_back("band" + std::to_string(i));
    for (std::size_t i = 0; i < kCtlCount; ++i) ctlKeys.push_back("ctl" + std::to_string(i));
    // A glyph's key names the control it belongs to and its slot in that control's text — never the
    // order it happened to be emitted in. An emission-order key shifts for every glyph after one whose
    // text changed length, and the interpolator then eases unrelated glyphs into each other.
    constexpr int kLabelSlots = 10, kValueSlots = 4;
    std::vector<std::vector<std::string>> labelKeys(kCtlCount), valueKeys(kCtlCount);
    for (std::size_t c = 0; c < kCtlCount; ++c) {
        for (int i = 0; i < kLabelSlots; ++i)
            labelKeys[c].push_back("lbl" + std::to_string(c) + "_" + std::to_string(i));
        for (int i = 0; i < kValueSlots; ++i)
            valueKeys[c].push_back("val" + std::to_string(c) + "_" + std::to_string(i));
    }

    // ── Static tilemaps: the backdrop field and the deck face, stamped 4x4 from 32px cells ───
    constexpr int kFieldCols = kViewW / kFieldPx, kFieldRows = (kViewH + kFieldPx - 1) / kFieldPx;
    constexpr int kBackW = kFieldCols * 4, kBackH = kFieldRows * 4;
    std::vector<TileCell> backCells(static_cast<std::size_t>(kBackW) * kBackH);
    for (int cy = 0; cy < kFieldRows; ++cy)
        for (int cx = 0; cx < kFieldCols; ++cx) {
            const auto base = static_cast<std::uint16_t>(
                backdrop[static_cast<std::size_t>((cx * 3 + cy * 5) % 8)].tile);
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    backCells[static_cast<std::size_t>(cy * 4 + dy) * kBackW + (cx * 4 + dx)] =
                        TileCell{.atlas = backdrop.atlasId,
                                 .tile = static_cast<std::uint16_t>(base + dx + dy * kFieldStride),
                                 .palette = palBackdrop};
        }

    constexpr int kDeckRows = (kViewH - kDeckTop + kFieldPx - 1) / kFieldPx;
    constexpr int kDeckW = kFieldCols * 4, kDeckH = kDeckRows * 4;
    std::vector<TileCell> deckCells(static_cast<std::size_t>(kDeckW) * kDeckH);
    for (int cy = 0; cy < kDeckRows; ++cy)
        for (int cx = 0; cx < kFieldCols; ++cx) {
            // Row 0 is the lit rail, the last row the shaded lip, everything between the plain face.
            // The face ramp is deliberately tight so its fine tooth stays subtle, so the trim takes its
            // contrast from its OWN palette rather than from a wider swing in the art.
            std::size_t cell = static_cast<std::size_t>((cx * 7 + cy) % 4);
            PaletteId   pal  = palDeck;
            if (cy == 0)                  { cell = 4; pal = palDeckRail; }
            else if (cy == kDeckRows - 1) { cell = 5; pal = palDeckShade; }
            const auto base = static_cast<std::uint16_t>(deck[cell].tile);
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    deckCells[static_cast<std::size_t>(cy * 4 + dy) * kDeckW + (cx * 4 + dx)] =
                        TileCell{.atlas = deck.atlasId,
                                 .tile = static_cast<std::uint16_t>(base + dx + dy * kFieldStride),
                                 .palette = pal};
        }

    // ── The text readout's own grid ──────────────────────────────────────────────────────────
    constexpr int kMonCols = 40, kMonRows = 26;
    constexpr int kMonW = kMonCols * 2, kMonH = kMonRows * 2;
    std::vector<TileCell> mon(static_cast<std::size_t>(kMonW) * kMonH,
                              TileCell{.atlas = font.atlasId, .tile = 0, .palette = palMono});
    auto clearMon = [&] {
        for (TileCell& c : mon) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palMono;
        }
    };
    auto write = [&](int col, int row, std::string_view text, PaletteId pal) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kMonCols || row < 0 || row >= kMonRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(text[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = mon[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile    = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette = pal;
                }
        }
    };

    Load        load;
    // A capture pins the dominant load knob from the command line, so a control run and a later
    // comparison do identical work rather than whatever the deck happened to be left on.
    if (capturing && capSprites >= 0) load.sprites = std::clamp(capSprites, 0, kMaxSprites);
    std::size_t pick = 0;         // the control the keyboard / pad cursor is on
    int         dragging = -1;    // the rotary the mouse is turning, or -1
    float       dragAccum = 0.0f;
    int         tick = 0;

    // Nudge one control. `steps` is in detents; a lamp flips on any nonzero step.
    auto adjust = [&](std::size_t which, int steps) {
        if (steps == 0) return;
        switch (static_cast<Ctl>(which)) {
            case Ctl::Sprites:
                load.sprites = std::clamp(load.sprites + steps * 100, 0, kMaxSprites); break;
            case Ctl::TileLayers:
                load.tileLayers = std::clamp(load.tileLayers + steps, 0, kMaxTileLayers); break;
            case Ctl::BelowLenses:
                load.belowLenses = std::clamp(load.belowLenses + steps * 2, 0, kMaxLenses); break;
            case Ctl::BlendRuns:
                load.blendRuns = std::clamp(load.blendRuns + steps, 0, kMaxRuns); break;
            case Ctl::RegionBloom: load.regionBloom = !load.regionBloom; break;
            case Ctl::Rotation:    load.rotation    = !load.rotation;    break;
            case Ctl::FrameRipple: load.frameRipple = !load.frameRipple; break;
            case Ctl::Monitor:     load.monitor     = !load.monitor;     break;
            case Ctl::Interpolation:
                load.interpolation = !load.interpolation;
                renderer.automaticInterpolation(load.interpolation);
                break;
            case Ctl::OutputGrid:
                load.outputGrid = !load.outputGrid;
                renderer.evaluationGrid(load.outputGrid ? EvaluationGrid::Output
                                                        : EvaluationGrid::Viewport);
                break;
            case Ctl::Count: break;
        }
    };

    loop.simTick([&](const InputState& in) {
        // Adds this tick's own cost to the pending frame's sim_ms on the way out, whatever path the body
        // takes.
        struct TickCost {
            std::chrono::steady_clock::time_point t0;
            double&                               out;
            ~TickCost() {
                out += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
            }
        } const capCost{std::chrono::steady_clock::now(), capSimMs};

        if (capturing && std::chrono::duration<double>(capCost.t0 - capStart).count() >= capSeconds)
            loop.exitRequest();

        ++tick;
        if (in.justPressed(Action::Up))   pick = (pick + kCtlCount - 1) % kCtlCount;
        if (in.justPressed(Action::Down)) pick = (pick + 1) % kCtlCount;
        adjust(pick, (in.justPressed(Action::More) ? 1 : 0) - (in.justPressed(Action::Less) ? 1 : 0));
        if (in.justPressed(Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());

        // The mouse works the deck directly: press a control to grab it, drag a rotary to turn it.
        const Vec2i cursor = in.cursor();
        if (in.mouseJustPressed(MouseButton::Left)) {
            for (std::size_t i = 0; i < kCtlCount; ++i) {
                const Point c  = controlCenter(i);
                const float r  = (kDeck[i].rotary ? kKnobPx : kLampPx) * 0.5f;
                const float dx = static_cast<float>(cursor.x) - c.x;
                const float dy = static_cast<float>(cursor.y) - c.y;
                if (dx * dx + dy * dy > r * r) continue;
                pick = i;
                if (kDeck[i].rotary) { dragging = static_cast<int>(i); dragAccum = 0.0f; }
                else                 adjust(i, 1);
                break;
            }
        }
        if (dragging >= 0) {
            if (!in.mouseHeld(MouseButton::Left)) {
                dragging = -1;
            } else {
                // Drag up to turn clockwise. A detent every 12 px keeps a slow drag controllable and a
                // fast one from sweeping the whole range.
                dragAccum += static_cast<float>(-in.cursorDelta().y);
                while (dragAccum >= 12.0f)  { adjust(static_cast<std::size_t>(dragging), 1);  dragAccum -= 12.0f; }
                while (dragAccum <= -12.0f) { adjust(static_cast<std::size_t>(dragging), -1); dragAccum += 12.0f; }
            }
        }
    });

    FrameDrawState      frame;
    std::vector<Sprite> blobSprites, lensSprites, ctlSprites, labelSprites;

    // Frame-rate accounting over a one-second window.
    Renderer::RenderStats mark = renderer.renderStats();
    std::uint64_t markCalls = 0, calls = 0;
    double        second = 0.0;
    std::uint64_t fps = 0, cbRate = 0, missed = 0;

    loop.renderLoop([&]() {
        ++calls;
        frame.layers.clear();
        frame.postEffects.clear();

        DrawLayer back{.key = "backdrop"};
        back.z       = 0;
        back.size    = PixelSize{kViewW, kViewH};
        back.content = TileContent{.widthInTiles = kBackW, .heightInTiles = kBackH,
                                   .cells = std::span<const TileCell>(backCells),
                                   .wrap = TileWrap::Blank};
        frame.layers.push_back(back);

        for (int i = 0; i < load.tileLayers; ++i) {
            DrawLayer band{.key = layerKeys[static_cast<std::size_t>(i)]};
            band.z       = 1 + i;
            band.size    = PixelSize{kViewW, kViewH};
            band.alpha   = 0.45f;
            band.scroll  = LayerScroll{.x = (tick * (i + 1)) % kViewW, .y = 0};
            band.content = TileContent{.widthInTiles = kBackW, .heightInTiles = kBackH,
                                       .cells = std::span<const TileCell>(backCells)};
            frame.layers.push_back(band);
        }

        blobSprites.clear();
        for (int i = 0; i < load.sprites; ++i) {
            const float phase = static_cast<float>(tick) * 0.01f + static_cast<float>(i) * 0.37f;
            const int   col = i % 48, row = i / 48;
            Sprite s{.key   = blobKeys[static_cast<std::size_t>(i)],
                     .x     = 16 + col * 39 + static_cast<int>(std::lround(std::sin(phase) * 12.0f)),
                     .y     = 120 + (row % 16) * 44 + static_cast<int>(std::lround(std::cos(phase) * 12.0f)),
                     .size  = AssetDimensions{.width = kFieldPx, .height = kFieldPx},
                     .atlas = blobs.atlasId,
                     .tile  = static_cast<std::uint16_t>(blobs[static_cast<std::size_t>(i % 8)].tile),
                     .palette = palBlob};
            if (load.rotation)
                s.transform = Transform::rotation(static_cast<float>(tick + i * 7) * 0.7f,
                                                  kFieldPx * 0.5f, kFieldPx * 0.5f);
            if (load.blendRuns > 0) {
                // Contiguous blocks, so the RUNS value IS the number of runs the layer splits into.
                // Interleaving instead would maximise the split for a given count and read as damage
                // rather than as the cost being dialled.
                const int block = (i * load.blendRuns) / std::max(1, load.sprites);
                if ((block & 1) != 0) s.blend = BlendMode::Screen;
            }
            if (load.regionBloom)
                s.regions = {Region{.key   = "core",
                                    .shape = ShapePoints::rectangle({0.0f, 0.0f},
                                                                    static_cast<float>(kFieldPx),
                                                                    static_cast<float>(kFieldPx)),
                                    .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                                                  .radius = 6.0f, .intensity = 255}}}};
            blobSprites.push_back(s);
        }
        DrawLayer loadLayer{.key = "load"};
        loadLayer.z       = 10;
        loadLayer.size    = PixelSize{kViewW, kViewH};
        // Per LAYER, not per frame: a frame effect would distort the deck and the readout along with
        // the scene, and a control you cannot read while it runs is no control.
        if (load.frameRipple)
            loadLayer.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple,
                                                   .amplitude = 8.0f, .frequency = 5.0f,
                                                   .phase = static_cast<float>(tick) * 0.01f,
                                                   .center = {kViewW / 2.0f, kViewH / 2.0f},
                                                   .decay = 1.6f}};
        loadLayer.content = SpriteContent{.sprites = std::span<const Sprite>(blobSprites)};
        frame.layers.push_back(loadLayer);

        lensSprites.clear();
        for (int i = 0; i < load.belowLenses; ++i) {
            const float phase = static_cast<float>(tick) * 0.006f + static_cast<float>(i) * 0.9f;
            Sprite l{.key   = lensKeys[static_cast<std::size_t>(i)],
                     .x     = kViewW / 2 - kFieldPx +
                              static_cast<int>(std::lround(std::sin(phase) * 760.0f)),
                     .y     = 300 + static_cast<int>(std::lround(std::cos(phase * 0.6f + i) * 220.0f)),
                     .size  = AssetDimensions{.width = kFieldPx, .height = kFieldPx},
                     .atlas = blobs.atlasId,
                     .tile  = static_cast<std::uint16_t>(blobs[0].tile),
                     .palette = palBlob};
            ScreenSpaceEffect glow{.kind = ScreenSpaceEffectKind::Glow,
                                   .fill = Rgba8{.r = 255, .g = 190, .b = 90, .a = 255},
                                   .radius = 10.0f, .threshold = 30, .intensity = 255};
            glow.scope = ScreenSpaceEffectScope::Below;
            l.effects  = {glow};
            lensSprites.push_back(l);
        }
        DrawLayer lensLayer{.key = "lenses"};
        lensLayer.z       = 20;
        lensLayer.size    = PixelSize{kViewW, kViewH};
        lensLayer.content = SpriteContent{.sprites = std::span<const Sprite>(lensSprites)};
        frame.layers.push_back(lensLayer);

        // ── The deck face ─────────────────────────────────────────────────────────────────────
        DrawLayer face{.key = "deck"};
        face.z       = 80;
        face.size    = PixelSize{kViewW, kViewH};
        face.scroll  = LayerScroll{.x = 0, .y = -kDeckTop};   // place the band along the bottom
        face.content = TileContent{.widthInTiles = kDeckW, .heightInTiles = kDeckH,
                                   .cells = std::span<const TileCell>(deckCells),
                                   .wrap = TileWrap::Blank};
        frame.layers.push_back(face);

        // Knobs and lamps. A rotary's pointer angle IS its value; a lamp's state is its palette.
        ctlSprites.clear();
        for (std::size_t i = 0; i < kCtlCount; ++i) {
            const Point c   = controlCenter(i);
            const bool  rot = kDeck[i].rotary;
            const int   px  = rot ? kKnobPx : kLampPx;
            Sprite s{.key   = ctlKeys[i],
                     .x     = static_cast<int>(c.x) - px / 2,
                     .y     = static_cast<int>(c.y) - px / 2,
                     .size  = AssetDimensions{.width = px, .height = px},
                     .atlas = rot ? knobs.atlasId : lamps.atlasId,
                     .tile  = static_cast<std::uint16_t>(rot ? knobs[0].tile : lamps[0].tile),
                     .palette = rot ? palKnob
                                    : (lampLit(load, static_cast<Ctl>(i)) ? palLampOn : palLampOff)};
            if (rot) {
                const float f = rotaryFraction(load, static_cast<Ctl>(i));
                s.transform = Transform::rotation((f * 2.0f - 1.0f) * kKnobSweep,
                                                  px * 0.5f, px * 0.5f);
            } else if (lampLit(load, static_cast<Ctl>(i))) {
                // A lit lamp actually glows — the same emission path the BLOOM control loads.
                s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                               .radius = 7.0f, .threshold = 90, .intensity = 200}};
            }
            ctlSprites.push_back(s);
        }
        DrawLayer controls{.key = "controls"};
        controls.z       = 85;
        controls.size    = PixelSize{kViewW, kViewH};
        controls.content = SpriteContent{.sprites = std::span<const Sprite>(ctlSprites)};
        frame.layers.push_back(controls);

        // Labels and values, as glyph sprites so they sit with their control.
        labelSprites.clear();
        // `keys` supplies one stable key per character slot, so a glyph keeps its identity whatever the
        // text says this frame.
        auto stamp = [&](int cx, int y, std::string_view text, PaletteId pal,
                         const std::vector<std::string>& keys) {
            const int w = static_cast<int>(text.size()) * kGlyphPx;
            for (std::size_t i = 0; i < text.size() && i < keys.size(); ++i) {
                if (text[i] == ' ') continue;
                labelSprites.push_back(
                    Sprite{.key   = keys[i],
                           .x     = cx - w / 2 + static_cast<int>(i) * kGlyphPx,
                           .y     = y,
                           .size  = AssetDimensions{.width = kGlyphPx, .height = kGlyphPx},
                           .atlas = font.atlasId,
                           .tile  = static_cast<std::uint16_t>(font[glyphCell(text[i])].tile),
                           .palette = pal});
            }
        };
        for (std::size_t i = 0; i < kCtlCount; ++i) {
            const Point     c   = controlCenter(i);
            const PaletteId pal = (i == pick) ? palFontPick : palFont;
            stamp(static_cast<int>(c.x), kLabelY, kDeck[i].label, pal, labelKeys[i]);
            if (kDeck[i].rotary) {
                const Ctl k = static_cast<Ctl>(i);
                const int v = k == Ctl::Sprites     ? load.sprites
                            : k == Ctl::TileLayers  ? load.tileLayers
                            : k == Ctl::BelowLenses ? load.belowLenses
                                                    : load.blendRuns;
                // Fixed width, so a digit rolling over never moves the slots either side of it.
                stamp(static_cast<int>(c.x), kDeckTop + 8,
                      pad(static_cast<std::uint64_t>(v), kValueSlots), pal, valueKeys[i]);
            }
        }
        DrawLayer labels{.key = "labels"};
        labels.z       = 86;
        labels.size    = PixelSize{kViewW, kViewH};
        labels.content = SpriteContent{.sprites = std::span<const Sprite>(labelSprites)};
        frame.layers.push_back(labels);

        // ── The text readout ──────────────────────────────────────────────────────────────────
        const Renderer::RenderStats st = renderer.renderStats();
        const auto& ph = st.lastFrame;
        second += 1.0 / 60.0;
        if (second >= 1.0) {
            fps       = st.presentPasses - mark.presentPasses;
            missed    = st.presentSkips - mark.presentSkips;
            cbRate    = calls - markCalls;
            mark      = st;
            markCalls = calls;
            second    = 0.0;
        }
        if (load.monitor) {
            const auto us = [](double ms) { return static_cast<std::uint64_t>(ms * 1000.0 + 0.5); };
            clearMon();
            write(0, 0, "RENDER STATS", palMonoPick);
            write(0, 2, "FPS " + pad(fps, 4) + "  CALLS " + pad(cbRate, 4) + "  MISSED " + pad(missed, 4),
                  palMono);
            write(0, 4, "MICROSECONDS THIS FRAME", palMono);
            write(2, 5, "ACQUIRE " + pad(us(ph.acquireMs), 6) + "  INTERP  " + pad(us(ph.interpMs), 6),
                  palMono);
            write(2, 6, "COMPOSE " + pad(us(ph.composeMs), 6) + "  PRESENT " + pad(us(ph.presentMs), 6),
                  palMono);
            write(0, 8, "ISSUED AND SKIPPED", palMono);
            write(2, 9, "TILES   " + pad(st.tilemapUploads, 8) + " " + pad(st.tilemapSkips, 8), palMono);
            write(2, 10, "SPRITES " + pad(st.spriteUploads, 8) + " " + pad(st.spriteSkips, 8), palMono);
            write(2, 11, "COMPOSE " + pad(st.composePasses, 8) + " " + pad(st.composeSkips, 8), palMono);
            write(2, 12, "PRESENT " + pad(st.presentPasses, 8) + " " + pad(st.presentSkips, 8), palMono);

            // The settings, as a menu. Same state the deck shows, read as a list — the selected row is
            // the one the arrows and the d-pad act on, and it tracks whatever the mouse last grabbed.
            write(0, 14, "SETTINGS", palMonoPick);
            for (std::size_t i = 0; i < kCtlCount; ++i) {
                const Ctl  k = static_cast<Ctl>(i);
                std::string value;
                if (kDeck[i].rotary) {
                    const int v = k == Ctl::Sprites     ? load.sprites
                                : k == Ctl::TileLayers  ? load.tileLayers
                                : k == Ctl::BelowLenses ? load.belowLenses
                                                        : load.blendRuns;
                    value = pad(static_cast<std::uint64_t>(v), 5);
                } else {
                    value = lampLit(load, k) ? "   ON" : "  OFF";
                }
                std::string row = kDeck[i].label;
                while (row.size() < 12) row.push_back(' ');
                write(2, 15 + static_cast<int>(i), row + value, (i == pick) ? palMonoPick : palMono);
            }

            DrawLayer monitor{.key = "monitor"};
            monitor.z       = 90;
            monitor.size    = PixelSize{kViewW, kViewH};
            monitor.scroll  = LayerScroll{.x = -24, .y = -24};
            monitor.content = TileContent{.widthInTiles = kMonW, .heightInTiles = kMonH,
                                          .cells = std::span<const TileCell>(mon),
                                          .wrap = TileWrap::Blank};
            frame.layers.push_back(monitor);
        }

        renderer.renderFrame(frame);

        // Read the stats for the frame that just went out, while it is still the last one.
        if (capturing && capRows.size() < capRows.capacity()) {
            const Renderer::RenderStats s = renderer.renderStats();
            const std::uint64_t         t = loop.tickCount();
            capRows.push_back(CaptureRow{
                .frame          = static_cast<std::uint64_t>(capRows.size()),
                .tNs            = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - capStart).count()),
                .ticks          = t - capLastTicks,
                .tickCount      = t,
                .acquireMs      = s.lastFrame.acquireMs,
                .interpMs       = s.lastFrame.interpMs,
                .composeMs      = s.lastFrame.composeMs,
                .presentMs      = s.lastFrame.presentMs,
                .presented      = s.lastFrame.presented ? 1 : 0,
                .composeSkipped = s.lastFrame.composeSkipped ? 1 : 0,
                .presentPasses  = s.presentPasses,
                .presentSkips   = s.presentSkips,
                .composePasses  = s.composePasses,
                .composeSkips   = s.composeSkips,
                .tilemapUploads = s.tilemapUploads,
                .spriteUploads  = s.spriteUploads,
                .scene          = 0,
                .simMs          = capSimMs,
                .sprites        = load.sprites});
            capLastTicks = t;
            capSimMs     = 0.0;
        }
    });

    WindowedHost host{loop, platform};
    host.run();
    if (capturing) writeCapture(capPath, capRows);
    return 0;
}
