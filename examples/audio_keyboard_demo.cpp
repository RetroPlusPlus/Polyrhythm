// Audio keyboard demo — a window of on-screen buttons you play like a little keyboard.
//
// The d-pad (or arrows / WASD) and the X / Z keys (pad south / east) each play a gentle triangle note
// (a C-major pentatonic: C D E G A, plus C an octave up). The pressed key lights up (a steady palette
// change — no flashing), and
// because each note is its OWN AudioSystem draining its own output stream, holding several buttons at
// once layers the notes into a chord — a direct showcase of the engine's "run as many independent
// audio systems as you like" model. No Vm, no register writes appear here: a sound is registered on the
// AudioLibrary and cued by handle on a system; the audio system hides everything underneath.
//
// (Why one AudioSystem per note: a faithful gentle triangle driver fills a VM's small code arena, so a
// system hosts one. That turns the limit into the feature — six independent systems = real polyphony.)
//
// Built on every CI platform so the audio + tile paths keep compiling against the live engine; never
// run in CI (no display, no audio device).

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "retropp/audio_library.h"  // AudioLibrary — registration lives here, not on a system
#include "retropp/audio_system.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

constexpr int kTile = 8;
constexpr int kMapW = 20, kMapH = 18;

// The demo's input vocabulary: one action per note.
enum class Action : std::uint8_t { NoteC, NoteD, NoteE, NoteG, NoteA, NoteC2 };

// Glyph atlas indices.
enum Glyph { GBlank, GUp, GDown, GLeft, GRight, GA, GB, GC, GD, GE, GG, GCount };

// A 5×7 font: 7 rows per glyph, low 5 bits per row (bit 4 = leftmost column). Direction arrows + the
// letters used as note names (C D E G A) and face-button labels (A B).
constexpr std::array<std::array<std::uint8_t, 7>, GCount> kFont{{
    {0, 0, 0, 0, 0, 0, 0},                                          // blank
    {0b00100, 0b01110, 0b10101, 0b00100, 0b00100, 0b00100, 0b00100},  // up arrow
    {0b00100, 0b00100, 0b00100, 0b00100, 0b10101, 0b01110, 0b00100},  // down arrow
    {0b00000, 0b00100, 0b01000, 0b11111, 0b01000, 0b00100, 0b00000},  // left arrow
    {0b00000, 0b00100, 0b00010, 0b11111, 0b00010, 0b00100, 0b00000},  // right arrow
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},  // B
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},  // C
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},  // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111},  // G
}};

// Build an indexed atlas: one 8×8 tile per glyph, index 1 = lit, index 0 = background. The 5×7 glyph
// sits with a 1-px left and 1-px top margin.
std::vector<std::uint8_t> buildFontAtlas() {
    const int width = kTile * GCount;
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * kTile, 0);
    for (int g = 0; g < GCount; ++g) {
        for (int row = 0; row < 7; ++row) {
            const std::uint8_t bits = kFont[static_cast<std::size_t>(g)][static_cast<std::size_t>(row)];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1) {
                    atlas[static_cast<std::size_t>(1 + row) * width + (g * kTile + 1 + col)] = 1;
                }
            }
        }
    }
    return atlas;
}

// One on-screen key: the action that triggers it, the glyph drawn for its identity (an arrow or a face
// letter) + the note-name glyph below it, the top-left grid cell, and which audio system plays it.
struct Key {
    Action action;
    int    idGlyph;
    int    noteGlyph;
    int    col;
    int    row;
    int    system;  // index into the per-note AudioSystem list
};

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Audio Keyboard Demo"},
        .window = {.title = "Polyrhythm — audio keyboard (d-pad + X/Z play notes)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The bindings: the directional preset lays the four cross notes on arrows + WASD + d-pad; the two
    // face-button notes sit on X / Z (pad south / east).
    ActionMap map{
        {Action::NoteA,  {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::NoteC2, {SDL_SCANCODE_Z, PadButton::FaceEast}},
    };
    map.add(presets::directional(Action::NoteC, Action::NoteD, Action::NoteE, Action::NoteG));
    platform.actions(map);

    // One AudioSystem (and its own output stream) per note — each gentle-triangle driver fills its own
    // VM, and the OS mixes the independent streams, so held notes sound together as a chord. Each system
    // auto-owns its production sink (the sink-less ctor), so there is no sink to declare or keep alive.
    std::vector<std::unique_ptr<AudioSystem>> systems;
    for (int i = 0; i < 6; ++i) {
        systems.push_back(std::make_unique<AudioSystem>(AudioKind::Chiptune, VMPlatform::GameBoyColor));
    }

    // Registration lives on the single AudioLibrary, NOT on a system — a system only CUES. Register the
    // assets ONCE (selecting their SM83 ISA), each by its full project-relative LITERAL path; the engine
    // resolves it against assetRoot() (set by setActive above, the executable's own location), and the
    // AudioIds are shared so each voice materializes its own driver copy when cued. Each note is a
    // complete, self-contained driver (channel init + trigger): a voice runs on its own fresh VM, so the
    // channel is never-triggered when wave RAM is written (safe) and the DAC turns on before the one
    // trigger (no pop) — a driver never depends on another routine having configured the chip.
    //
    // This demo is the EXHAUSTIVE policy test: a few notes are Embed (baked into the binary by the build
    // scan — the .asm never ships) and the rest are LoadFromPath (the .asm rides along beside the binary
    // and is assembled at startup). Both resolve to identical sound; the only difference is where the
    // bytes come from. (Chiptune's per-type default is Embed, but the policy is written out here so the
    // split is explicit.)
    // ONE register call per statement — the build scan keys each call to its own `;`, so a call buried
    // in a multi-element initializer would not be seen individually (the asset-scan convention).
    AudioLibrary& library = AudioLibrary::instance();
    // Embed (baked into the binary, .asm never ships): the first two notes.
    const AudioId toneC    = library.registerAudio("examples/assets/tones/tone_c.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    const AudioId toneD    = library.registerAudio("examples/assets/tones/tone_d.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    // LoadFromPath (the .asm rides along beside the binary): the remaining four notes.
    const AudioId toneE    = library.registerAudio("examples/assets/tones/tone_e.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneG    = library.registerAudio("examples/assets/tones/tone_g.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneA    = library.registerAudio("examples/assets/tones/tone_a.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const AudioId toneC2   = library.registerAudio("examples/assets/tones/tone_c2.asm",   AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
    const std::array<AudioId, 6> toneIds{toneC, toneD, toneE, toneG, toneA, toneC2};

    const std::vector<std::uint8_t> atlas = buildFontAtlas();
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * GCount, kTile).atlasId;
    // Two palettes sharing a dark background: index 1 is the glyph color — dim when idle, bright when
    // the key is held. Switching a cell's palette is the steady highlight (no animation, no flashing).
    const std::array<Rgba8, 4> idleColours{{{18, 20, 30}, {96, 120, 112}, {0, 0, 0}, {0, 0, 0}}};
    const std::array<Rgba8, 4> heldColours{{{18, 20, 30}, {244, 230, 120}, {0, 0, 0}, {0, 0, 0}}};
    const PaletteId idlePal = renderer.uploadPalette(std::span<const Rgba8>(idleColours));
    const PaletteId heldPal = renderer.uploadPalette(std::span<const Rgba8>(heldColours));

    // The keyboard layout: a d-pad cross on the left, the two face-button notes on the right. Each key
    // shows its identity glyph with the note name directly below.
    const std::array<Key, 6> keys{{
        {Action::NoteC,  GUp,    GC, 4, 4, 0},   // Up            → C
        {Action::NoteE,  GLeft,  GE, 2, 6, 2},   // Left          → E
        {Action::NoteG,  GRight, GG, 6, 6, 3},   // Right         → G
        {Action::NoteD,  GDown,  GD, 4, 8, 1},   // Down          → D
        {Action::NoteA,  GA,     GA, 13, 5, 4},  // X / pad south → A
        {Action::NoteC2, GB,     GC, 15, 7, 5},  // Z / pad east  → C (octave up)
    }};

    // Lay the glyphs into the tile grid once; only their palette (idle/held) changes per frame. Every
    // cell names the font atlas + its current palette directly.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (TileCell& c : cells) {
        c.tile = GBlank;
        c.atlas = atlasId;
        c.palette = idlePal;
    }
    auto idCell   = [&](const Key& k) -> TileCell& { return cells[static_cast<std::size_t>(k.row) * kMapW + k.col]; };
    auto noteCell = [&](const Key& k) -> TileCell& { return cells[static_cast<std::size_t>(k.row + 1) * kMapW + k.col]; };
    for (const Key& k : keys) {
        idCell(k).tile   = static_cast<std::uint16_t>(k.idGlyph);
        noteCell(k).tile = static_cast<std::uint16_t>(k.noteGlyph);
    }

    FrameDrawState frame;
    frame.layers.push_back(DrawLayer{.key = "keyboard"});
    DrawLayer& layer = frame.layers[0];
    layer.z       = 0;
    layer.size    = PixelSize{config.viewport.width, config.viewport.height};
    layer.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                .cells = std::span<const TileCell>(cells)};

    loop.simTick([&](const InputState& in) {
        for (const Key& k : keys) {
            const auto sys = static_cast<std::size_t>(k.system);
            if (in.justPressed(k.action)) {
                systems[sys]->play(toneIds[sys]);  // a fresh self-contained voice: init + trigger
            }
            if (in.justReleased(k.action)) {
                systems[sys]->stop();
            }
            const PaletteId pal = in.isHeld(k.action) ? heldPal : idlePal;
            idCell(k).palette   = pal;
            noteCell(k).palette = pal;
        }
        // No per-frame audio stepping — each AudioSystem produces on its own thread; pressing
        // a key cues its note above, releasing stops it.
    });
    loop.renderLoop([&]() { renderer.renderFrame(frame); });

    std::printf("Audio keyboard — press the d-pad (arrows / WASD) and X / Z (pad south / east) to play "
                "notes; hold several for a chord. Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
