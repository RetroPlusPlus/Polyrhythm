#pragma once

// Vantium — the ASSET LOADER, its own translation unit. Slices the three committed indexed PNGs,
// uploads every (≤8-entry) palette, and builds the shared Animation clips: the explosion flipbook,
// the fuel-pod glow pulse, and the starfield twinkle (the latter two are pure PALETTE animation —
// the art never changes, the colors breathe). Everything is Embed; see assets.cpp.

#include <array>
#include <cstdint>
#include <vector>

#include "retropp/animation.h"  // Animation
#include "retropp/image.h"      // AtlasManifest / ContentKind / ReadOrder
#include "retropp/palette.h"    // PaletteId
#include "retropp/renderer.h"   // Renderer

#include "layout.h"

namespace vant {

struct VantAssets {
    retropp::AtlasManifest font;    // rich 16×16 glyph cells (2×2 tile stamps)
    retropp::AtlasManifest tiles;   // the 16×16 deck-art strip (2×2 tile stamps; set-pieces span more)
    retropp::AtlasManifest sheet;   // 48×24 SpriteSeries: manta ×4 / fighter / mine / bolt / eshot / boom ×4

    std::vector<retropp::PaletteId>    spritePals;   // indexed by Pal
    std::array<retropp::PaletteId, 11> tilePals{};   // indexed by TilePal
    std::array<retropp::PaletteId, 3>  textPals{};   // indexed by TextPal

    retropp::Animation boomClip;      // the 4-frame explosion, played single() per instance
    retropp::Animation podPulse;      // 2 palette frames over the pod art — the glow breathes
    retropp::Animation starTwinkle;   // 2 palette frames over the star art — the field shimmers

    [[nodiscard]] retropp::AtlasId fontAtlas()   const { return font.atlasId; }
    [[nodiscard]] retropp::AtlasId tileAtlas()   const { return tiles.atlasId; }
    [[nodiscard]] retropp::AtlasId spriteAtlas() const { return sheet.atlasId; }

    [[nodiscard]] std::uint16_t slotTile(Slot s) const {
        return sheet[static_cast<std::size_t>(s)].tile;
    }
    // A 16×16 art cell's top-left engine tile; its 2×2 group offsets by (+dx, +kTileStride8·dy).
    [[nodiscard]] std::uint16_t artTile(TileArt a) const {
        return tiles[static_cast<std::size_t>(a)].tile;
    }
    // A rich glyph's top-left engine tile (digits, A–Z, space for anything unmapped).
    [[nodiscard]] std::uint16_t glyphBase(char ch) const {
        std::size_t k = 36;
        if (ch >= '0' && ch <= '9')      k = static_cast<std::size_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') k = static_cast<std::size_t>(10 + (ch - 'A'));
        return font[k].tile;
    }
    [[nodiscard]] std::uint16_t ruleBase() const { return font[font.tileCount() - 1].tile; }
};

// Load + slice the sheets, upload every palette, build the shared clips. A plain value — the clips'
// frames name their sheets by AtlasId, so the assets object returns, moves, and copies freely.
[[nodiscard]] VantAssets loadVantAssets(retropp::Renderer& renderer);

}  // namespace vant
