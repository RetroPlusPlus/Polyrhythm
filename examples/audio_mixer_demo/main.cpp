// Audio mixer demo — four on-screen volume sliders you adjust live and hear.
//
// The program-wide AudioMixer carries a Master level plus one per bus — Music, SFX, Vocals — each a 0..255
// slider that scales every AudioSystem's output through a perceptual taper (half the slider sounds like
// half). This window makes that audible: a sustained Music tone (C) and a Vocals tone (G) you toggle, a
// one-shot SFX blip you fire, and four bars showing the current levels. Pull a bus down and only that
// bus quiets; pull Master down and everything quiets together — the whole point of a mixer.
//
// Each tone is its own AudioSystem draining its own stream, exactly the "run as many audio systems as you
// like" model; the mixer is the one shared object they all read their levels from. No Vm, no register
// writes appear here — audio is registered on the AudioLibrary and cued by handle, and the levels are set
// on AudioMixer::instance().
//
// Controls: Up/Down select a slider, Left/Right adjust it, X toggles the Music tone, Z toggles the Vocals
// tone, Enter fires an SFX blip. Built on every CI platform so the path keeps compiling; never run in CI
// (no display, no audio device). Dev-run only; static layout, the only motion is a bar length and a
// highlight, so it is safe to leave running.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/audio_library.h"  // AudioLibrary — registration lives here, not on a system
#include "retropp/audio_mixer.h"    // AudioMixer — the shared volume levels this demo drives
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

// The demo's input vocabulary: bus navigation, volume nudges, and the three sound triggers.
enum class Action : std::uint8_t {
    PrevBus, NextBus, VolumeDown, VolumeUp, ToggleMusic, ToggleVocals, FireSfx,
};

constexpr int kTile = 8;
constexpr int kMapW = 20, kMapH = 18;

// Glyph atlas indices. GBar is a fully-lit tile (a bar cell); the rest are 5×7 letters spelling the four
// bus labels. Index 1 is the lit color, index 0 the background — so a palette swap recolors any glyph.
enum Glyph { GBlank, GBar, GM, GA, GS, GT, GE, GR, GU, GI, GC, GF, GX, GV, GO, GL, GCount };

// A 5×7 uppercase font (7 rows, low 5 bits per row, bit 4 = leftmost column) for the letters in
// MASTER / MUSIC / SFX / VOCALS. GBlank and GBar carry no glyph bits (GBar is filled solid below).
constexpr std::array<std::array<std::uint8_t, 7>, GCount> kFont{{
    {0, 0, 0, 0, 0, 0, 0},                                             // GBlank
    {0, 0, 0, 0, 0, 0, 0},                                             // GBar (filled solid in buildAtlas)
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001},   // M
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},   // A
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110},   // S
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},   // T
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},   // E
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},   // R
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},   // U
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111},   // I
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},   // C
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000},   // F
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},   // X
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100},   // V
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},   // O
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111},   // L
}};

// Build an indexed atlas: one 8×8 tile per glyph, index 1 = lit. Letters sit with a 1-px left/top margin;
// GBar is filled solid so a bar cell is a full block the palette colors (fill vs track).
std::vector<std::uint8_t> buildFontAtlas() {
    const int width = kTile * GCount;
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(width) * kTile, 0);
    for (int y = 0; y < kTile; ++y) {  // GBar: a fully-lit tile the bar palettes color
        for (int x = 0; x < kTile; ++x) {
            atlas[static_cast<std::size_t>(y) * width + GBar * kTile + x] = 1;
        }
    }
    for (int g = GM; g < GCount; ++g) {  // the letters
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

// One bus row: its label text and the grid row it draws on. Bus 0 is Master (scales everything); rows
// 1/2/3 are the Music/SFX/Vocals AudioType buses.
struct Bus {
    const char* label;
    int         row;
};

// Map a label character to its glyph index.
int glyphFor(char c) {
    switch (c) {
        case 'M': return GM;
        case 'A': return GA;
        case 'S': return GS;
        case 'T': return GT;
        case 'E': return GE;
        case 'R': return GR;
        case 'U': return GU;
        case 'I': return GI;
        case 'C': return GC;
        case 'F': return GF;
        case 'X': return GX;
        case 'V': return GV;
        case 'O': return GO;
        case 'L': return GL;
        default:  return GBlank;
    }
}

constexpr int kBusCount = 4;      // Master, Music, SFX, Vocals
constexpr int kBarCol   = 7;      // bars start here, past the widest 6-letter label
constexpr int kBarLen   = kMapW - kBarCol;  // 13 tiles of bar track
constexpr int kStep     = 16;     // Left/Right nudge per press (0..255 in ~16 steps)

// Apply a bus's level to the mixer. Bus 0 is Master; 1/2/3 are the Music/SFX/Vocals AudioType buses. Each
// hands over an AudioLevels aggregate naming just the one channel it changes; the rest are left as they are.
void applyLevel(int bus, std::uint8_t level) {
    AudioMixer& m = AudioMixer::instance();
    switch (bus) {
        case 0: m.levels(AudioLevels{.master = level}); break;
        case 1: m.levels(AudioLevels{.music = level}); break;
        case 2: m.levels(AudioLevels{.sfx = level}); break;
        case 3: m.levels(AudioLevels{.vocals = level}); break;
        default: break;
    }
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Audio Mixer Demo"},
        .window = {.title = "Polyrhythm — audio mixer (Up/Down select, Left/Right adjust)"}};
    EngineConfig::setActive(config);  // the bare AudioSystem/Renderer ctors below inherit it

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ToggleMusic,  {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::ToggleVocals, {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {Action::FireSfx,      {SDL_SCANCODE_RETURN, PadButton::Start}},
    };
    map.add(presets::directional(Action::PrevBus, Action::NextBus, Action::VolumeDown, Action::VolumeUp));
    platform.actions(map);

    // Three independent chiptune systems, each auto-owning its output sink. They share the one AudioMixer:
    // the Music system's tone is scaled by Master × Music, the Vocals system's by Master × Vocals, the SFX
    // blip by Master × SFX. Pull one bus down and only that stream quiets.
    AudioSystem musicSys{AudioKind::Chiptune, VMPlatform::GameBoyColor};
    AudioSystem vocalsSys{AudioKind::Chiptune, VMPlatform::GameBoyColor};
    AudioSystem sfxSys{AudioKind::Chiptune, VMPlatform::GameBoyColor};

    // Registration lives on the single AudioLibrary; each system only cues. Every tone is a complete,
    // self-contained driver (channel init + trigger) — each cued voice runs on its own fresh VM, so a
    // driver never depends on another routine having configured the chip. One register call per
    // statement — the build scan keys each call to its own `;`.
    AudioLibrary& library = AudioLibrary::instance();
    const AudioId toneC    = library.registerAudio("examples/assets/tones/tone_c.asm",    AudioType::Music, Isa::Sm83, AssetPolicy::Embed);
    const AudioId toneG    = library.registerAudio("examples/assets/tones/tone_g.asm",    AudioType::Vocals, Isa::Sm83, AssetPolicy::Embed);
    const AudioId sfxBlip  = library.registerAudio("examples/assets/tones/sfx_blip.asm",  AudioType::Sfx, Isa::Sm83, AssetPolicy::Embed);

    const std::vector<std::uint8_t> atlas = buildFontAtlas();
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * GCount, kTile).atlasId;
    // Index 1 is the drawn color; index 0 the shared dark background. Four palettes: dim label, bright
    // selected label, bright bar fill, dark bar track.
    const std::array<Rgba8, 4> dimCols{{{18, 20, 30}, {96, 110, 120}, {0, 0, 0}, {0, 0, 0}}};
    const std::array<Rgba8, 4> selCols{{{18, 20, 30}, {244, 230, 120}, {0, 0, 0}, {0, 0, 0}}};
    const std::array<Rgba8, 4> fillCols{{{18, 20, 30}, {120, 220, 140}, {0, 0, 0}, {0, 0, 0}}};
    const std::array<Rgba8, 4> trackCols{{{18, 20, 30}, {40, 48, 58}, {0, 0, 0}, {0, 0, 0}}};
    const PaletteId dimPal   = renderer.uploadPalette(std::span<const Rgba8>(dimCols));
    const PaletteId selPal   = renderer.uploadPalette(std::span<const Rgba8>(selCols));
    const PaletteId fillPal  = renderer.uploadPalette(std::span<const Rgba8>(fillCols));
    const PaletteId trackPal = renderer.uploadPalette(std::span<const Rgba8>(trackCols));

    const std::array<Bus, kBusCount> buses{{
        {"MASTER", 3},
        {"MUSIC",  6},
        {"SFX",    9},
        {"VOCALS", 12},
    }};

    // Lay every label glyph into the grid once (only palettes change per frame); the bar cells are placed
    // fresh each frame from the level. Every cell names the font atlas directly.
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (TileCell& c : cells) {
        c.tile    = GBlank;
        c.atlas   = atlasId;
        c.palette = dimPal;
    }
    for (const Bus& b : buses) {
        for (int i = 0; b.label[i] != '\0'; ++i) {
            TileCell& cell = cells[static_cast<std::size_t>(b.row) * kMapW + i];
            cell.tile      = static_cast<std::uint16_t>(glyphFor(b.label[i]));
        }
    }

    FrameDrawState frame;
    frame.layers.push_back(DrawLayer{.key = "mixer"});
    DrawLayer& layer = frame.layers[0];
    layer.z       = 0;
    layer.size    = PixelSize{config.viewport.width, config.viewport.height};
    layer.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                .cells = std::span<const TileCell>(cells)};

    // Levels start at unity (255) — the faithful default, full volume — so the taper is heard by pulling
    // down from the top. Mirror them here and push each to the mixer as it changes.
    std::array<std::uint8_t, kBusCount> levels{255, 255, 255, 255};
    for (int b = 0; b < kBusCount; ++b) {
        applyLevel(b, levels[static_cast<std::size_t>(b)]);
    }
    int  selected = 0;
    bool musicOn  = false;
    bool vocalsOn = false;

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::PrevBus)) {
            selected = (selected + kBusCount - 1) % kBusCount;
        }
        if (in.justPressed(Action::NextBus)) {
            selected = (selected + 1) % kBusCount;
        }
        if (in.justPressed(Action::VolumeDown)) {
            int lvl = levels[static_cast<std::size_t>(selected)] - kStep;
            levels[static_cast<std::size_t>(selected)] = static_cast<std::uint8_t>(std::max(lvl, 0));
            applyLevel(selected, levels[static_cast<std::size_t>(selected)]);
        }
        if (in.justPressed(Action::VolumeUp)) {
            int lvl = levels[static_cast<std::size_t>(selected)] + kStep;
            levels[static_cast<std::size_t>(selected)] = static_cast<std::uint8_t>(std::min(lvl, 255));
            applyLevel(selected, levels[static_cast<std::size_t>(selected)]);
        }
        if (in.justPressed(Action::ToggleMusic)) {
            musicOn = !musicOn;
            if (musicOn) { musicSys.play(toneC); } else { musicSys.stop(); }
        }
        if (in.justPressed(Action::ToggleVocals)) {
            vocalsOn = !vocalsOn;
            if (vocalsOn) { vocalsSys.play(toneG); } else { vocalsSys.stop(); }
        }
        if (in.justPressed(Action::FireSfx)) {
            sfxSys.play(sfxBlip);  // one-shot; the Sfx bus auto-closes it when it goes silent
        }

        // Repaint the label highlight (selected bus bright) and the bar (filled proportional to level).
        for (int b = 0; b < kBusCount; ++b) {
            const Bus& bus = buses[static_cast<std::size_t>(b)];
            const PaletteId labelPal = (b == selected) ? selPal : dimPal;
            for (int i = 0; bus.label[i] != '\0'; ++i) {
                cells[static_cast<std::size_t>(bus.row) * kMapW + i].palette = labelPal;
            }
            const int filled =
                (static_cast<int>(levels[static_cast<std::size_t>(b)]) * kBarLen + 127) / 255;
            for (int x = 0; x < kBarLen; ++x) {
                TileCell& cell = cells[static_cast<std::size_t>(bus.row) * kMapW + kBarCol + x];
                cell.tile      = GBar;
                cell.palette   = (x < filled) ? fillPal : trackPal;
            }
        }
    });
    loop.renderLoop([&]() { renderer.renderFrame(frame); });

    std::printf(
        "Audio mixer — Up/Down select a slider, Left/Right adjust it.\n"
        "  X toggles the Music tone (C), Z toggles the Vocals tone (G), Enter fires an SFX blip.\n"
        "  Pull one bus down and only it quiets; pull MASTER down and everything quiets together.\n"
        "  Levels start at full (unity); half-slider sounds about half (a perceptual taper).\n"
        "  Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
