#pragma once

// The demo's panel: eight voice columns. This file is pure UI — layout, mouse hit-testing, drawing — and
// knows nothing about the audio system. main.cpp maps a clicked column to a cue or a stop.
//
// Drawing is two pure functions: voiceRegions() returns the colored rectangles — a pad per voice, lit
// while it sounds, and a bar for its pitch — as a list of Regions, and textSprites() returns the labels as
// glyph Sprites. main.cpp puts the regions on a low-z layer and the sprites on a higher-z text layer.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"

namespace demo {

using namespace retropp;

constexpr int kViewW  = 816;
constexpr int kViewH  = 520;
constexpr int kVoices = 8;

// What the panel shows, per voice, and which voice the keys act on.
struct PanelState {
    std::array<bool, kVoices>             keyed{};     // sounding
    std::array<std::uint16_t, kVoices>    pitch{};     // the voice's pitch register, $0000-$3FFF
    std::array<std::string_view, kVoices> wave{};      // the name of the sample it plays
    std::array<std::string_view, kVoices> effect{};    // the effect on its cue: echo, reverb or none
    std::array<std::string, kVoices>      mode{};      // how its cue plays: CONT, ONCE, or REP and a tempo
    std::array<std::uint8_t, kVoices>     volume{};    // $00-$7F
    std::array<int, kVoices>              pan{};       // -8 (left) to 8 (right), 0 the middle
    int                                   selected = 0;
};

// What the mouse is on: a voice's pad, its pitch bar, its volume bar, its pan track or its wave name. The
// bars and the track take a drag; `valueAt` answers what a cursor on one means, 0..1 along it.
enum class Part : std::uint8_t { None, Pad, Pitch, Volume, Pan, Wave };
struct Hit {
    Part part  = Part::None;
    int  voice = -1;
};
[[nodiscard]] Hit   hitTest(Vec2i cursor);
[[nodiscard]] float valueAt(Part part, int voice, Vec2i cursor);

// The font atlas + the palettes the panel draws text in.
struct Fonts {
    AtlasManifest font;
    PaletteId     text;    // labels
    PaletteId     active;  // live values, the selected voice
    PaletteId     dim;     // secondary notes
};

// The colored rectangles of the panel, in draw order.
[[nodiscard]] std::vector<Region> voiceRegions(const PanelState& s);
// The labels and the live readouts as glyph sprites.
[[nodiscard]] std::vector<Sprite> textSprites(const PanelState& s, const Fonts& fonts);

}  // namespace demo
