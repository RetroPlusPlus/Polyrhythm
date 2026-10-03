#pragma once

// The demo's control surface — a media-player faceplate for one hosted SNES sound driver. This file is pure
// UI (layout, mouse hit-testing, drawing); it knows nothing about audio. main.cpp maps a clicked Control to
// the matching handle verb.
//
// Drawing is two pure functions: controlRegions() returns the colored rectangles (pads, buttons, the note
// strip, the meters) as a list of Regions, and textSprites() returns the labels as glyph Sprites. main.cpp
// puts the regions on a low-z faceplate layer and the sprites on a higher-z text layer, so the labels sit
// above the panel.

#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"

namespace demo {

using namespace retropp;

constexpr int kViewW = 816;
constexpr int kViewH = 520;

// Every interactive control: the three song pads, the effect pad, FADE, EJECT and the output fader.
enum class Control : std::uint8_t { None, Song0, Song1, Song2, Effect, Fade, Eject, Output };

// The song a pad plays (the driver's own song number), or -1 for any other control.
[[nodiscard]] int padSong(Control c);

// What the driver reports, read back from slots() each tick. A negative value is one not read yet.
struct PanelState {
    int     song     = -1;     // the song playing, 0 to 2; 255 when none
    int     note     = -1;     // the note playing, 0 to 7
    int     volume   = -1;     // the master volume, 0 to $60
    int     effect   = -1;     // the effect's ticks left, 0 to 6
    int     frames   = -1;     // the frames the driver's tick has run, as a byte
    bool    resident = true;   // the driver is hosted; EJECT closes it
    float   output   = 1.0f;   // the vmDriver mixer bus, 0..1
    Control flash    = Control::None;  // a pad pressed this frame (drawn lit)
};

// The control under `cursor`, or Control::None.
[[nodiscard]] Control hitTest(Vec2i cursor);
// The 0..1 value the output fader takes for a cursor at `cursor` on its track.
[[nodiscard]] float outputValueAt(Vec2i cursor);

// The font atlas + the palettes the panel draws text in.
struct Fonts {
    AtlasManifest font;
    PaletteId     text;    // labels
    PaletteId     active;  // live values, lit controls
    PaletteId     dim;     // secondary notes
};

// The colored rectangles of the faceplate, in draw order.
[[nodiscard]] std::vector<Region> controlRegions(const PanelState& s);
// The text labels + live readouts as glyph sprites.
[[nodiscard]] std::vector<Sprite> textSprites(const PanelState& s, const Fonts& fonts);

}  // namespace demo
