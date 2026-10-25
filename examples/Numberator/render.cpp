#include "render.h"

#include <cstddef>
#include <span>

#include "layout.h"

namespace numberator {

using namespace retropp;

namespace {
constexpr int kGlyphW = 24;  // a font cell is 24 wide
}

void View::build(FrameDrawState& frame, const Assets& a, const Sprite& closeBox,
                 const std::string& display, int pressedKey) {
    frame.layers.clear();

    // z=0 — the chrome: window body, title bar, and the sunken display well, assembled from the map PNG.
    DrawLayer chrome{.key = "chrome"};
    chrome.z       = 0;
    chrome.size    = PixelSize{kViewW, kViewH};
    chrome.content = a.chromeMap.asTileContent(TileWrap::Blank);
    frame.layers.push_back(std::move(chrome));

    // z=10 — the key sprites, plus the close box (a sprite so the app can query its shape — the click
    // test and the drag-handle geometry both read the same value). A held key flips X and Y, so its
    // raised bevel inverts to a sunken one.
    keys_.clear();
    keys_.push_back(closeBox);
    // Stable per-index sprite keys (required + unique frame-wide), built once so the views stay valid.
    static const std::vector<std::string> keyNames =
        [] { std::vector<std::string> v; for (int k = 0; k < 64; ++k) v.push_back("key" + std::to_string(k)); return v; }();
    static const std::vector<std::string> lblNames =
        [] { std::vector<std::string> v; for (int k = 0; k < 64; ++k) v.push_back("lbl" + std::to_string(k)); return v; }();
    static const std::vector<std::string> dispNames =
        [] { std::vector<std::string> v; for (int k = 0; k < 64; ++k) v.push_back("disp" + std::to_string(k)); return v; }();

    for (int k = 0; k < static_cast<int>(kKeys.size()); ++k) {
        const Key&       key  = kKeys[static_cast<std::size_t>(k)];
        const AssetSlot& slot = a.buttonSlots[key.isOp ? 1 : 0];
        const bool       down = (k == pressedKey);
        keys_.push_back(Sprite{.key = keyNames[static_cast<std::size_t>(k)],
                               .x = keyX(k % kCols), .y = keyY(k / kCols),
                               .size = slot.dimensions, .atlas = a.buttons,
                               .tile = slot.tile, .palette = a.palette, .flipX = down, .flipY = down});
    }
    DrawLayer keyLayer{.key = "keys"};
    keyLayer.z       = 10;
    keyLayer.size    = PixelSize{kViewW, kViewH};
    keyLayer.content = SpriteContent{std::span<const Sprite>(keys_)};
    frame.layers.push_back(std::move(keyLayer));

    // z=20 — the glyphs: each key's label centered on it, plus the right-aligned display digits. The font's
    // background index is the palette's alpha-0 entry, so the black ink composites over keys and the well.
    glyphs_.clear();
    for (int k = 0; k < static_cast<int>(kKeys.size()); ++k) {
        const Key&       key   = kKeys[static_cast<std::size_t>(k)];
        const AssetSlot& glyph = a.glyphSlots[static_cast<std::size_t>(glyphSlot(key.glyph))];
        const int        nudge = (k == pressedKey) ? 2 : 0;  // the label sinks with the held key
        glyphs_.push_back(Sprite{.key = lblNames[static_cast<std::size_t>(k)],
                                 .x = keyX(k % kCols) + (kBtnW - glyph.dimensions.width) / 2 + nudge,
                                 .y = keyY(k / kCols) + (kBtnH - glyph.dimensions.height) / 2 + nudge,
                                 .size = glyph.dimensions, .atlas = a.font,
                                 .tile = glyph.tile, .palette = a.palette});
    }
    // The display string, right-aligned; a glyph that would spill past the well's left edge is dropped.
    const int rightEdge = kDispX + kDispW - 8;
    const int n         = static_cast<int>(display.size());
    const int startX    = rightEdge - n * kGlyphW;
    const int dispY     = kDispY + (kDispH - 32) / 2;
    for (int i = 0; i < n; ++i) {
        const int x = startX + i * kGlyphW;
        if (x < kDispX + 2) continue;
        const int slotIndex = glyphSlot(display[static_cast<std::size_t>(i)]);
        if (slotIndex < 0 || slotIndex >= static_cast<int>(a.glyphSlots.size())) continue;
        const AssetSlot& glyph = a.glyphSlots[static_cast<std::size_t>(slotIndex)];
        glyphs_.push_back(Sprite{.key = dispNames[static_cast<std::size_t>(i)],
                                 .x = x, .y = dispY, .size = glyph.dimensions, .atlas = a.font,
                                 .tile = glyph.tile, .palette = a.palette});
    }
    DrawLayer glyphLayer{.key = "glyphs"};
    glyphLayer.z       = 20;
    glyphLayer.size    = PixelSize{kViewW, kViewH};
    glyphLayer.content = SpriteContent{std::span<const Sprite>(glyphs_)};
    frame.layers.push_back(std::move(glyphLayer));
}

}  // namespace numberator
