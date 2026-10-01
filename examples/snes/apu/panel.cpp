#include "panel.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace demo {
namespace {

// ── Geometry ──────────────────────────────────────────────────────────────────────────────────────
struct Rect {
    float x, y, w, h;
    [[nodiscard]] bool contains(Vec2i p) const {
        const auto fx = static_cast<float>(p.x), fy = static_cast<float>(p.y);
        return fx >= x && fx < x + w && fy >= y && fy < y + h;
    }
};

// Eight columns across the panel: a pad at the top, the pitch bar under it, the labels under that.
constexpr float kLeft = 24, kColumnW = 88, kColumnStep = 96;
constexpr float kPadY = 84, kPadH = 44;
constexpr float kBarY = 148, kBarH = 160;
constexpr float kVolumeY = 324, kPanY = 348, kStripH = 12;
constexpr float kWaveY = 376;
constexpr float kBarMax = 0x1000;  // the pitch that fills the bar: the sample at its own rate

[[nodiscard]] Rect column(int v)   { return {kLeft + v * kColumnStep, 60, kColumnW, 400}; }
[[nodiscard]] Rect pad(int v)      { return {kLeft + v * kColumnStep, kPadY, kColumnW, kPadH}; }
[[nodiscard]] Rect barTrack(int v) { return {kLeft + v * kColumnStep + 32, kBarY, 24, kBarH}; }
[[nodiscard]] Rect volumeTrack(int v) { return {kLeft + v * kColumnStep + 8, kVolumeY, kColumnW - 16, kStripH}; }
[[nodiscard]] Rect panTrack(int v)    { return {kLeft + v * kColumnStep + 8, kPanY, kColumnW - 16, kStripH}; }
[[nodiscard]] Rect waveName(int v)    { return {kLeft + v * kColumnStep, kWaveY - 4, kColumnW, 24}; }

// ── Colors ────────────────────────────────────────────────────────────────────────────────────────
constexpr Rgba8 kBackdrop{18, 20, 28};
constexpr Rgba8 kPanel{30, 34, 46};
constexpr Rgba8 kSelected{52, 60, 84};
constexpr Rgba8 kPadIdle{40, 46, 64};
constexpr Rgba8 kPadKeyed{80, 220, 255};
constexpr Rgba8 kTrack{40, 46, 64};
constexpr Rgba8 kBarIdle{90, 100, 130};
constexpr Rgba8 kBarKeyed{250, 200, 80};
constexpr Rgba8 kVolumeFill{90, 160, 240};
constexpr Rgba8 kKnob{214, 222, 240};

[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 color) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = color};
}
[[nodiscard]] Region rect(std::string key, Rect r, Rgba8 color) {
    return Region{.key     = std::move(key),
                  .shape   = ShapePoints::rectangle(Point{r.x, r.y}, r.w, r.h),
                  .effects = {solidFill(color)}};
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

// Four uppercase hex digits.
[[nodiscard]] std::string hex4(unsigned v) {
    static const char* d = "0123456789ABCDEF";
    return std::string{d[(v >> 12) & 0xF], d[(v >> 8) & 0xF], d[(v >> 4) & 0xF], d[v & 0xF]};
}

}  // namespace

Hit hitTest(Vec2i cursor) {
    for (int v = 0; v < kVoices; ++v) {
        if (!column(v).contains(cursor)) continue;
        if (pad(v).contains(cursor)) return {Part::Pad, v};
        // The bars and the track grab from a band a little wider than they are drawn.
        const Rect b = barTrack(v), vol = volumeTrack(v), p = panTrack(v);
        if (Rect{b.x - 8, b.y, b.w + 16, b.h}.contains(cursor)) return {Part::Pitch, v};
        if (Rect{vol.x, vol.y - 6, vol.w, vol.h + 12}.contains(cursor)) return {Part::Volume, v};
        if (Rect{p.x, p.y - 6, p.w, p.h + 12}.contains(cursor)) return {Part::Pan, v};
        if (waveName(v).contains(cursor)) return {Part::Wave, v};
        return {Part::None, v};
    }
    return {};
}

float valueAt(Part part, int voice, Vec2i cursor) {
    const auto fx = static_cast<float>(cursor.x), fy = static_cast<float>(cursor.y);
    switch (part) {
        case Part::Pitch: {  // up is more
            const Rect b = barTrack(voice);
            return std::clamp((b.y + b.h - fy) / b.h, 0.0f, 1.0f);
        }
        case Part::Volume: {
            const Rect v = volumeTrack(voice);
            return std::clamp((fx - v.x) / v.w, 0.0f, 1.0f);
        }
        case Part::Pan: {
            const Rect p = panTrack(voice);
            return std::clamp((fx - p.x) / p.w, 0.0f, 1.0f);
        }
        default:
            return 0.0f;
    }
}

std::vector<Region> voiceRegions(const PanelState& s) {
    std::vector<Region> r;
    r.push_back(rect("bg", {0, 0, kViewW, kViewH}, kBackdrop));
    r.push_back(rect("panel", {16, 50, 784, 420}, kPanel));
    for (int v = 0; v < kVoices; ++v) {
        const std::string n = std::to_string(v);
        if (v == s.selected) r.push_back(rect("sel" + n, column(v), kSelected));
        r.push_back(rect("pad" + n, pad(v), s.keyed[v] ? kPadKeyed : kPadIdle));
        // The pitch bar fills from the bottom, full at the sample's own rate.
        const Rect  t    = barTrack(v);
        const float fill = std::clamp(static_cast<float>(s.pitch[v]) / kBarMax, 0.0f, 1.0f) * t.h;
        r.push_back(rect("track" + n, t, kTrack));
        r.push_back(rect("bar" + n, {t.x, t.y + t.h - fill, t.w, fill}, s.keyed[v] ? kBarKeyed : kBarIdle));
        // The volume as a bar filling from the left, and the pan as a marker on a track, the middle at 0.
        const Rect vol = volumeTrack(v);
        r.push_back(rect("volTrack" + n, vol, kTrack));
        r.push_back(rect("vol" + n, {vol.x, vol.y, vol.w * static_cast<float>(s.volume[v]) / 0x7F, vol.h}, kVolumeFill));
        const Rect      p     = panTrack(v);
        constexpr float knobW = 6.0f;
        const float     knobX = p.x + (static_cast<float>(s.pan[v]) + 8.0f) / 16.0f * (p.w - knobW);
        r.push_back(rect("panTrack" + n, p, kTrack));
        r.push_back(rect("panMid" + n, {p.x + p.w / 2 - 1, p.y, 2, p.h}, kBarIdle));
        r.push_back(rect("pan" + n, {knobX, p.y - 2, knobW, p.h + 4}, kKnob));
    }
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

    center({0, 10, kViewW, kGlyphPx}, "SNES AUDIO UNIT", fonts.active, "title");
    center({0, 30, kViewW, kGlyphPx}, "EIGHT VOICES - NO 65816 PROGRAM", fonts.dim, "sub");

    for (int v = 0; v < kVoices; ++v) {
        const std::string n   = std::to_string(v);
        const Rect        c   = column(v);
        const PaletteId   pal = v == s.selected ? fonts.active : fonts.text;
        center({c.x, 62, c.w, kGlyphPx}, "V" + n, pal, "name" + n + "_");
        center(pad(v), s.keyed[v] ? "ON" : "OFF", s.keyed[v] ? fonts.active : fonts.dim, "pad" + n + "_");
        center({c.x, kWaveY, c.w, kGlyphPx}, s.wave[v], pal, "wave" + n + "_");
        center({c.x, 398, c.w, kGlyphPx}, hex4(s.pitch[v]), fonts.dim, "pitch" + n + "_");
    }
    // Two prefixes a digit can never run together into one key: "hintA" + "20" and "hintB" + "0" stay apart.
    center({0, 470, kViewW, kGlyphPx}, "1-8 KEY   LEFT RIGHT SELECT   UP DOWN PITCH", fonts.text, "hintA");
    center({0, 492, kViewW, kGlyphPx}, "TAB WAVE   - = VOLUME   BRACKETS PAN", fonts.text, "hintB");
    return out;
}

}  // namespace demo
