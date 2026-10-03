#include "panel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace demo {
namespace {

// ── Geometry ──────────────────────────────────────────────────────────────────────────────────────
// A rectangle in viewport pixels, with a point test for hit-testing.
struct Rect {
    float x, y, w, h;
    [[nodiscard]] bool contains(Vec2i p) const {
        const auto fx = static_cast<float>(p.x), fy = static_cast<float>(p.y);
        return fx >= x && fx < x + w && fy >= y && fy < y + h;
    }
};

constexpr float kLeft = 32, kPadW = 232, kPadH = 44, kPadStep = 248;
constexpr float kSongY = 80, kVerbY = 162;
constexpr float kNoteY = 296, kNoteW = 88, kNoteH = 28, kNoteStep = 94;
constexpr float kMeterW = 752;
constexpr float kLedY = 420, kLedSize = 20;

constexpr std::array<std::string_view, 3> kSongNames{"ARPEGGIO", "SCALE", "BASS"};

[[nodiscard]] Rect songPad(int i) { return {kLeft + i * kPadStep, kSongY, kPadW, kPadH}; }
[[nodiscard]] Rect verbPad(int i) { return {kLeft + i * kPadStep, kVerbY, kPadW, kPadH}; }
[[nodiscard]] Rect noteBox(int i) { return {kLeft + i * kNoteStep, kNoteY, kNoteW, kNoteH}; }
[[nodiscard]] Rect volumeMeter()  { return {kLeft, 360, kMeterW, 16}; }
[[nodiscard]] Rect effectMeter()  { return {kLeft + 240, 424, kMeterW - 240, 12}; }
[[nodiscard]] Rect led()          { return {kLeft, kLedY, kLedSize, kLedSize}; }
[[nodiscard]] Rect outputTrack()  { return {24, 494, 768, 16}; }

// ── Colors ────────────────────────────────────────────────────────────────────────────────────────
constexpr Rgba8 kBackdrop  {18, 20, 28};
constexpr Rgba8 kPanel     {30, 34, 46};
constexpr Rgba8 kPadIdle   {52, 60, 84};
constexpr Rgba8 kPadFlash  {80, 220, 255};
constexpr Rgba8 kPadPlaying{60, 130, 110};
constexpr Rgba8 kStopPad   {190, 78, 68};
constexpr Rgba8 kEjectPad  {150, 92, 58};
constexpr Rgba8 kNoteLit   {250, 200, 80};
constexpr Rgba8 kLedOn     {80, 230, 120};
constexpr Rgba8 kLedOff    {40, 50, 60};
constexpr Rgba8 kTrack     {40, 46, 64};
constexpr Rgba8 kVolumeFill{90, 160, 240};
constexpr Rgba8 kEffectFill{230, 110, 200};
constexpr Rgba8 kKnob      {214, 222, 240};

[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 color) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = color};
}
[[nodiscard]] Region rect(std::string key, Rect r, Rgba8 color) {
    return Region{.key     = std::move(key),
                  .shape   = ShapePoints::rectangle(Point{r.x, r.y}, r.w, r.h),
                  .effects = {solidFill(color)}};
}

// A track with a proportional fill.
void drawMeter(std::vector<Region>& r, const std::string& key, Rect t, float value, Rgba8 fill) {
    r.push_back(rect(key + "Track", t, kTrack));
    r.push_back(rect(key + "Fill", {t.x, t.y, std::clamp(value, 0.0f, 1.0f) * t.w, t.h}, fill));
}

// ── Text ──────────────────────────────────────────────────────────────────────────────────────────
constexpr int kGlyphPx = 16;

[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    switch (ch) {  // punctuation cells in the authored font sheet
        case '(': return 44;
        case ')': return 45;
        case '.': return 38;
        case '-': return 39;
        case '_': return 37;
        default:  return 36;  // space + punctuation the font has no glyph for
    }
}

// Two uppercase hex digits, or "--" for a value not read yet.
[[nodiscard]] std::string hex2(int v) {
    if (v < 0) return "--";
    static const char* d = "0123456789ABCDEF";
    return std::string{d[(v >> 4) & 0xF], d[v & 0xF]};
}

}  // namespace

int padSong(Control c) {
    switch (c) {
        case Control::Song0: return 0;
        case Control::Song1: return 1;
        case Control::Song2: return 2;
        default:             return -1;
    }
}

Control hitTest(Vec2i cursor) {
    for (int i = 0; i < 3; ++i) {
        if (songPad(i).contains(cursor)) return static_cast<Control>(static_cast<int>(Control::Song0) + i);
    }
    if (verbPad(0).contains(cursor)) return Control::Effect;
    if (verbPad(1).contains(cursor)) return Control::Fade;
    if (verbPad(2).contains(cursor)) return Control::Eject;
    // The fader grabs from a band a little taller than its track.
    const Rect o = outputTrack();
    if (Rect{o.x, o.y - 8, o.w, o.h + 16}.contains(cursor)) return Control::Output;
    return Control::None;
}

float outputValueAt(Vec2i cursor) {
    const Rect t = outputTrack();
    return std::clamp((static_cast<float>(cursor.x) - t.x) / t.w, 0.0f, 1.0f);
}

std::vector<Region> controlRegions(const PanelState& s) {
    std::vector<Region> r;
    r.push_back(rect("bg", {0, 0, kViewW, kViewH}, kBackdrop));
    r.push_back(rect("panel", {16, 50, 784, 405}, kPanel));

    // A song pad is lit the frame it is pressed and held green while its song plays.
    for (int i = 0; i < 3; ++i) {
        const Control c     = static_cast<Control>(static_cast<int>(Control::Song0) + i);
        const Rgba8   color = s.flash == c ? kPadFlash : s.song == i ? kPadPlaying : kPadIdle;
        r.push_back(rect("song" + std::to_string(i), songPad(i), color));
    }
    r.push_back(rect("effect", verbPad(0), s.flash == Control::Effect ? kPadFlash : kPadIdle));
    r.push_back(rect("fade", verbPad(1), kStopPad));
    r.push_back(rect("eject", verbPad(2), kEjectPad));

    // The eight notes of the song, the one playing lit.
    for (int i = 0; i < 8; ++i) {
        const bool lit = s.song >= 0 && s.song <= 2 && s.note == i;
        r.push_back(rect("note" + std::to_string(i), noteBox(i), lit ? kNoteLit : kTrack));
    }
    drawMeter(r, "volume", volumeMeter(), static_cast<float>(std::max(s.volume, 0)) / 0x60, kVolumeFill);
    drawMeter(r, "effectMeter", effectMeter(), static_cast<float>(std::max(s.effect, 0)) / 6, kEffectFill);
    r.push_back(rect("led", led(), s.resident ? kLedOn : kLedOff));

    const Rect t = outputTrack();
    drawMeter(r, "output", t, s.output, kVolumeFill);
    constexpr float knobW = 12.0f;
    r.push_back(rect("outputKnob", {t.x + std::clamp(s.output, 0.0f, 1.0f) * (t.w - knobW), t.y - 4, knobW, t.h + 8},
                     kKnob));
    return r;
}

std::vector<Sprite> textSprites(const PanelState& s, const Fonts& fonts) {
    std::vector<Sprite> out;
    // Draw `text` left-aligned at (x, y). `keyPrefix` gives each glyph slot a stable key across frames, so
    // a value rolling over never re-identifies its neighbors.
    const auto write = [&](int x, int y, std::string_view text, PaletteId pal, std::string_view keyPrefix) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (glyphCell(text[i]) == 36) continue;  // spaces + punctuation the font has no glyph for
            out.push_back(Sprite{
                .key     = std::string(keyPrefix) + std::to_string(i),
                .x       = x + static_cast<int>(i) * kGlyphPx,
                .y       = y,
                .size    = AssetDimensions{.width = kGlyphPx, .height = kGlyphPx},
                .atlas   = fonts.font.atlasId,
                .tile    = static_cast<std::uint16_t>(fonts.font[glyphCell(text[i])].tile),
                .palette = pal});
        }
    };
    const auto center = [&](Rect box, std::string_view text, PaletteId pal, std::string_view keyPrefix) {
        write(static_cast<int>(box.x + box.w / 2) - static_cast<int>(text.size()) * kGlyphPx / 2,
              static_cast<int>(box.y + (box.h - kGlyphPx) / 2), text, pal, keyPrefix);
    };
    const int x = static_cast<int>(kLeft);

    center({0, 10, kViewW, kGlyphPx}, "SNES SOUND DRIVER", fonts.active, "title");
    center({0, 30, kViewW, kGlyphPx}, "65816 DRIVER - SPC700 PROGRAM", fonts.dim, "sub");

    write(x, 58, "SONGS  PLAY(ID)", fonts.text, "songLbl");
    for (int i = 0; i < 3; ++i) {
        center(songPad(i), std::to_string(i) + " " + std::string{kSongNames[static_cast<std::size_t>(i)]},
               fonts.active, "song" + std::to_string(i));
    }
    write(x, 140, "SLOTS(COMMAND)", fonts.text, "effectLbl");
    write(x + static_cast<int>(kPadStep), 140, "STOP()", fonts.text, "fadeLbl");
    write(x + static_cast<int>(2 * kPadStep), 140, "CLOSE()", fonts.text, "ejectLbl");
    center(verbPad(0), "EFFECT", fonts.active, "effect");
    center(verbPad(1), "FADE", fonts.text, "fade");
    center(verbPad(2), "EJECT", fonts.text, "eject");

    write(x, 224, "SLOTS()", fonts.dim, "readLbl");
    write(x, 244,
          "SONG " + hex2(s.song) + "  NOTE " + hex2(s.note) + "  VOL " + hex2(s.volume) + "  FX " +
              hex2(s.effect) + "  FRAMES " + hex2(s.frames),
          fonts.active, "read");
    write(x, 276, "THE NOTE PLAYING", fonts.text, "noteLbl");
    write(x, 340, "MASTER VOLUME", fonts.text, "volLbl");
    write(x + 26, static_cast<int>(kLedY) + 2, s.resident ? "RESIDENT" : "CLOSED", fonts.dim, "ledLbl");
    write(x + 240, 400, "THE EFFECT", fonts.text, "fxLbl");
    center({0, 472, kViewW, kGlyphPx}, "OUTPUT  VMDRIVER MIXER BUS", fonts.text, "outLbl");
    return out;
}

}  // namespace demo
