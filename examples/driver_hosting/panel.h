#pragma once

// The demo's control surface — a media-player faceplate with two IDENTICAL driver columns side by side, so
// the mirror image itself shows the feature's promise: the same controls drive either family. This file is
// pure UI (layout, mouse hit-testing, drawing); it knows nothing about audio. main.cpp maps a clicked
// Control to the matching handle verb.
//
// Drawing is two pure functions: controlRegions() returns the colored rectangles (pads, faders, LEDs) as a
// list of Regions, and textSprites() returns the labels as glyph Sprites. main.cpp puts the regions on a
// low-z faceplate layer and the sprites on a higher-z text layer, so the labels sit above the panel.

#include <cstdint>
#include <string>
#include <vector>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"

namespace demo {

using namespace retropp;

constexpr int kViewW = 816;
constexpr int kViewH = 520;

// Every interactive control. The two driver columns (L = left, R = right) carry the SAME set of controls —
// four music pads, three sfx pads, STOP, RESET, EJECT, a volume fader — laid out identically. Output is the
// one shared control (the mixer bus over both drivers). The L block is 1..11 and the R block 12..22, so a
// control's side and its per-column role are pure arithmetic (see side()/local() in the .cpp).
enum class Control : std::uint8_t {
    None = 0,
    LMus0, LMus1, LMus2, LMus3, LSfx0, LSfx1, LSfx2, LStop, LEject, LVol, LReset,  // 1..11
    RMus0, RMus1, RMus2, RMus3, RSfx0, RSfx1, RSfx2, RStop, REject, RVol, RReset,  // 12..22
    Output,                                                                        // 23
};

// The driver sound id a pad cues (the driver's own sound number, not an engine AudioId). 0 for non-pads.
[[nodiscard]] std::uint8_t padSoundId(Control c);

// The side (0 = left, 1 = right) a control belongs to, or -1 for None/Output.
[[nodiscard]] int side(Control c);
// The per-column role (0..3 music, 4..6 sfx, 7 stop, 8 eject, 9 volume, 10 reset), or -1.
[[nodiscard]] int local(Control c);

// One driver column's live values, drawn identically on either side.
struct ColumnState {
    std::string title;                 // "RAM-FLAG DRIVER" / "ARGUMENT DRIVER"
    std::string mechanism;             // the one-line family difference, e.g. "PLAY(ID) TO A MAILBOX"
    int   musicLastSeen = -1;          // slots() read-back; < 0 draws as "--"
    int   sfxLastSeen   = -1;
    int   volume        = -1;
    bool  resident      = true;
    float vol           = 1.0f;        // the volume fader position, 0..1
};

struct PanelState {
    ColumnState left;
    ColumnState right;
    float       output = 1.0f;         // the shared vmDriver mixer bus, 0..1
    Control     flash  = Control::None;  // a pad cued this frame (drawn lit)
};

// The control under `cursor`, or Control::None.
[[nodiscard]] Control hitTest(Vec2i cursor);
// Whether `c` is a fader (a click-and-drag control).
[[nodiscard]] bool isFader(Control c);
// The 0..1 value a fader takes for a cursor at `cursor` on its track.
[[nodiscard]] float faderValueAt(Control fader, Vec2i cursor);

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
