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

constexpr float kPadW = 84, kPadH = 44, kColStep = 90;
constexpr float kMusY = 118, kSfxY = 196, kBtnY = 378, kVolY = 342;
constexpr float kBtnW = 84, kBtnH = 36, kLedSize = 20;

// A column's left origin (pads / buttons / fader) and its text origin.
[[nodiscard]] float colX(int s)  { return s == 0 ? 24.0f : 428.0f; }
[[nodiscard]] float textX(int s) { return colX(s) + 4.0f; }

[[nodiscard]] Rect panelBox(int s) { return {colX(s) - 8, 50, 384, 405}; }
[[nodiscard]] Rect musicPad(int s, int i) { return {colX(s) + i * kColStep, kMusY, kPadW, kPadH}; }
[[nodiscard]] Rect sfxPad(int s, int i)   { return {colX(s) + i * kColStep, kSfxY, kPadW, kPadH}; }
[[nodiscard]] Rect stopBtn(int s)  { return {colX(s), kBtnY, kBtnW, kBtnH}; }
[[nodiscard]] Rect resetBtn(int s) { return {colX(s) + kColStep, kBtnY, kBtnW, kBtnH}; }
[[nodiscard]] Rect ejectBtn(int s) { return {colX(s) + 2 * kColStep, kBtnY, kBtnW, kBtnH}; }
[[nodiscard]] Rect led(int s)      { return {colX(s) + 3 * kColStep + 8, kBtnY + 8, kLedSize, kLedSize}; }
[[nodiscard]] Rect volTrack(int s) { return {colX(s), kVolY, 352, 12}; }
[[nodiscard]] Rect outputTrack()   { return {24, 494, 768, 16}; }

// The Control for a column control by side (0/1) and role (0..9).
[[nodiscard]] Control ctlOf(int s, int role) {
    return static_cast<Control>(1 + s * 11 + role);
}

// ── Colors ───────────────────────────────────────────────────────────────────────────────────────
constexpr Rgba8 kBackdrop  {18, 20, 28};
constexpr Rgba8 kPanel     {30, 34, 46};
constexpr Rgba8 kPadIdle   {52, 60, 84};
constexpr Rgba8 kPadFlash  {80, 220, 255};
constexpr Rgba8 kStopPad   {190, 78, 68};
constexpr Rgba8 kEjectPad  {150, 92, 58};
constexpr Rgba8 kLedOn     {80, 230, 120};
constexpr Rgba8 kLedOff    {40, 50, 60};
constexpr Rgba8 kTrack     {40, 46, 64};
constexpr Rgba8 kFaderFill {90, 160, 240};
constexpr Rgba8 kKnob      {214, 222, 240};

[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}
[[nodiscard]] Region rect(std::string key, Rect r, Rgba8 colour) {
    return Region{.key = std::move(key),
                  .shape = ShapePoints::rectangle(Point{r.x, r.y}, r.w, r.h),
                  .effects = {solidFill(colour)}};
}

// ── Text ──────────────────────────────────────────────────────────────────────────────────────────
constexpr int kGlyphPx = 16;

[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    switch (ch) {                 // punctuation cells in the authored font sheet
        case '(': return 44;
        case ')': return 45;
        case '.': return 38;
        case '-': return 39;
        case '_': return 37;
        default:  return 36;      // space + punctuation the font has no glyph for
    }
}

// Two uppercase hex digits, or "--" for a negative (nothing-seen) value.
[[nodiscard]] std::string hex2(int v) {
    if (v < 0) return "--";
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.push_back(d[(v >> 4) & 0xF]);
    s.push_back(d[v & 0xF]);
    return s;
}

}  // namespace

std::uint8_t padSoundId(Control c) {
    static constexpr std::array<std::uint8_t, 4> kMusic{0x30, 0x60, 0x90, 0xC0};
    static constexpr std::array<std::uint8_t, 3> kSfx{0x40, 0x80, 0xC0};
    const int l = local(c);
    if (l >= 0 && l <= 3) return kMusic[static_cast<std::size_t>(l)];
    if (l >= 4 && l <= 6) return kSfx[static_cast<std::size_t>(l - 4)];
    return 0;
}

int side(Control c) {
    const int v = static_cast<int>(c);
    if (v >= 1 && v <= 11) return 0;
    if (v >= 12 && v <= 22) return 1;
    return -1;
}
int local(Control c) {
    const int v = static_cast<int>(c);
    return (v >= 1 && v <= 22) ? (v - 1) % 11 : -1;
}

bool isFader(Control c) {
    return c == Control::LVol || c == Control::RVol || c == Control::Output;
}

Control hitTest(Vec2i cursor) {
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 4; ++i)
            if (musicPad(s, i).contains(cursor)) return ctlOf(s, i);
        for (int i = 0; i < 3; ++i)
            if (sfxPad(s, i).contains(cursor)) return ctlOf(s, 4 + i);
        if (stopBtn(s).contains(cursor))  return ctlOf(s, 7);
        if (resetBtn(s).contains(cursor)) return ctlOf(s, 10);
        if (ejectBtn(s).contains(cursor)) return ctlOf(s, 8);
        // A fader grabs from a band a little taller than its track.
        const Rect t = volTrack(s);
        if (Rect{t.x, t.y - 8, t.w, t.h + 16}.contains(cursor)) return ctlOf(s, 9);
    }
    const Rect o = outputTrack();
    if (Rect{o.x, o.y - 8, o.w, o.h + 16}.contains(cursor)) return Control::Output;
    return Control::None;
}

float faderValueAt(Control fader, Vec2i cursor) {
    const Rect t = fader == Control::Output ? outputTrack() : volTrack(side(fader));
    return std::clamp((static_cast<float>(cursor.x) - t.x) / t.w, 0.0f, 1.0f);
}

namespace {

// A fader's track + proportional fill + knob.
void drawFader(std::vector<Region>& r, const std::string& key, Rect t, float value) {
    r.push_back(rect(key + "Track", t, kTrack));
    r.push_back(rect(key + "Fill", {t.x, t.y, std::max(0.0f, value * t.w), t.h}, kFaderFill));
    constexpr float knobW = 12.0f;
    const float knobX = t.x + std::clamp(value, 0.0f, 1.0f) * (t.w - knobW);
    r.push_back(rect(key + "Knob", {knobX, t.y - 4, knobW, t.h + 8}, kKnob));
}

// One driver column's rectangles (pads, buttons, LED, volume fader). Identical on either side.
void drawColumn(std::vector<Region>& r, int s, const ColumnState& cs, Control flash) {
    const std::string p = s == 0 ? "l" : "r";
    for (int i = 0; i < 4; ++i)
        r.push_back(rect(p + "mus" + std::to_string(i), musicPad(s, i),
                         flash == ctlOf(s, i) ? kPadFlash : kPadIdle));
    for (int i = 0; i < 3; ++i)
        r.push_back(rect(p + "sfx" + std::to_string(i), sfxPad(s, i),
                         flash == ctlOf(s, 4 + i) ? kPadFlash : kPadIdle));
    r.push_back(rect(p + "stop", stopBtn(s), kStopPad));
    r.push_back(rect(p + "reset", resetBtn(s), kStopPad));
    r.push_back(rect(p + "eject", ejectBtn(s), kEjectPad));
    r.push_back(rect(p + "led", led(s), cs.resident ? kLedOn : kLedOff));
    drawFader(r, p + "vol", volTrack(s), cs.vol);
}

}  // namespace

std::vector<Region> controlRegions(const PanelState& s) {
    std::vector<Region> r;
    r.push_back(rect("bg", {0, 0, kViewW, kViewH}, kBackdrop));
    r.push_back(rect("leftPanel", panelBox(0), kPanel));
    r.push_back(rect("rightPanel", panelBox(1), kPanel));
    drawColumn(r, 0, s.left, s.flash);
    drawColumn(r, 1, s.right, s.flash);
    drawFader(r, "output", outputTrack(), s.output);
    return r;
}

namespace {

// Draw one driver column's labels + live readout, identical on either side. `write`/`center` close over the
// output sprite list in the caller.
void drawColumnText(int s, const ColumnState& cs, const Fonts& fonts, auto&& write, auto&& center) {
    const int x = static_cast<int>(textX(s));
    const std::string p = s == 0 ? "l" : "r";
    write(x, 58, cs.title, fonts.text, p + "title");
    write(x, 78, cs.mechanism, fonts.dim, p + "mech");
    write(x, 100, "MUSIC  PLAY(ID)", fonts.text, p + "musLbl");
    write(x, 178, "SFX  PLAY(ID)", fonts.text, p + "sfxLbl");
    for (int i = 0; i < 4; ++i)
        center(static_cast<int>(colX(s) + i * kColStep + kPadW / 2), static_cast<int>(kMusY) + 14,
               hex2(padSoundId(ctlOf(s, i))), fonts.active, p + "musId" + std::to_string(i));
    for (int i = 0; i < 3; ++i)
        center(static_cast<int>(colX(s) + i * kColStep + kPadW / 2), static_cast<int>(kSfxY) + 14,
               hex2(padSoundId(ctlOf(s, 4 + i))), fonts.active, p + "sfxId" + std::to_string(i));
    write(x, 258, "SLOTS()", fonts.dim, p + "readLbl");
    write(x, 278, "MUS " + hex2(cs.musicLastSeen) + " SFX " + hex2(cs.sfxLastSeen) + " VOL " +
                      hex2(cs.volume),
          fonts.active, p + "read");
    write(x, 316, "DRIVER VOL  SLOTS(VOL)", fonts.text, p + "volLbl");
    center(static_cast<int>(stopBtn(s).x + kBtnW / 2), static_cast<int>(kBtnY) + 10, "STOP", fonts.text,
           p + "stopLbl");
    center(static_cast<int>(resetBtn(s).x + kBtnW / 2), static_cast<int>(kBtnY) + 10, "RESET",
           fonts.text, p + "resetLbl");
    center(static_cast<int>(ejectBtn(s).x + kBtnW / 2), static_cast<int>(kBtnY) + 10, "EJECT",
           fonts.text, p + "ejectLbl");
    write(static_cast<int>(led(s).x + 26), static_cast<int>(kBtnY) + 10,
          cs.resident ? "RESIDENT" : "CLOSED", fonts.dim, p + "ledLbl");
}

}  // namespace

std::vector<Sprite> textSprites(const PanelState& s, const Fonts& fonts) {
    std::vector<Sprite> out;
    // Draw `text` left-aligned at (x, y). `keyPrefix` gives each glyph slot a stable key across frames, so
    // a value rolling over never re-identifies its neighbors.
    const auto write = [&](int x, int y, std::string_view text, PaletteId pal,
                           std::string_view keyPrefix) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (glyphCell(text[i]) == 36) continue;  // spaces + punctuation (the font has no glyph)
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
    const auto center = [&](int cx, int y, std::string_view text, PaletteId pal,
                            std::string_view keyPrefix) {
        write(cx - static_cast<int>(text.size()) * kGlyphPx / 2, y, text, pal, keyPrefix);
    };

    center(kViewW / 2, 10, "HOSTED SOUND DRIVERS", fonts.active, "title");
    center(kViewW / 2, 30, "SAME VERBS - DIFFERENT REGISTRATION", fonts.dim, "sub");
    drawColumnText(0, s.left, fonts, write, center);
    drawColumnText(1, s.right, fonts, write, center);
    center(kViewW / 2, 472, "OUTPUT  VMDRIVER MIXER BUS (BOTH DRIVERS)", fonts.text, "outLbl");
    return out;
}

}  // namespace demo
