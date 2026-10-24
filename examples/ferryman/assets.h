#pragma once

// Ferryman — the ASSET LOADER, its own translation unit. Slices the three committed indexed PNGs,
// loads EVERY palette from a PALETTE IMAGE (a 16x1 RGBA PNG under assets/palettes/, one pixel per
// entry, alpha included — loadPaletteImage; no color table is hand-written in C++), and builds
// the shared Animation clips: the ferry's thruster flicker, the three colonist idle bobs, the
// abductor's wing-light blink, the mutant pulse, the explosion flipbook, the beacon's glow cycle
// + the water shimmer (both pure PALETTE animation — the art holds, the colors breathe), and the
// vehicle running-light phase. Everything is Embed — the built binary is self-contained; see
// assets.cpp for the policy, decided per call.

#include <array>
#include <cstdint>
#include <vector>

#include "retropp/animation.h"  // Animation — the shared clips live beside the art they use
#include "retropp/image.h"      // AtlasManifest / ContentKind / ReadOrder
#include "retropp/palette.h"    // PaletteId
#include "retropp/renderer.h"   // Renderer (loadAtlas + loadPaletteImage)

#include "layout.h"

namespace ferryman {

struct FerrymanAssets {
    retropp::AtlasManifest font;     // rich 16×16 glyph cells (2×2 tile stamps) + the rule
    retropp::AtlasManifest terrain;  // 32×32 Tileset: water/shore/lane/median/sanctuary tiles
    retropp::AtlasManifest sheet;    // 48×48 SpriteSeries: the 19 game sprites
    retropp::AtlasManifest title;    // the bespoke 32×32 "FERRYMAN" glyphs (gold over the waterline)

    // Every palette below came off a palette IMAGE (assets/palettes/<name>.png).
    std::vector<retropp::PaletteId>   spritePals;   // indexed by the Pal enum (layout.h)
    std::array<retropp::PaletteId, 8> terrainPals{};  // indexed by TerrainPal
    // Two liveries per text color, differing ONLY in entry 0 (the glyph cell's background):
    //   textPals — entry 0 is ALPHA-0 (a material hole): text floats over whatever is behind it.
    //   hudPals  — entry 0 is the opaque HUD-bar color: the band's fill, text, and rule.
    std::array<retropp::PaletteId, 3> textPals{};  // indexed by TextPal
    std::array<retropp::PaletteId, 3> hudPals{};   // indexed by TextPal
    retropp::PaletteId                titlePal{};  // the title set's own livery (alpha-0 backed)

    // The reality-warp wake — a game-registered CUSTOM shader stage (registered by literal path so
    // the build scan compiles it). Shared by the ferry (distortion only) and the mutant (distortion
    // + a psychedelic hue-cycle, via its warpChroma param). Runs on a below-the-mover layer.
    retropp::PostProcessStageId       wakeWarp{};

    // The shared clips. Frame clips vary the ART (thruster, bobs, wings, pulse, boom); the
    // beacon + shimmer clips vary only the PALETTE; the lights clip is read for its frame INDEX
    // (the render maps livery → that phase's palette). All pure data — the feel layer owns the
    // players that walk them, per the game-owns-the-cursor contract.
    retropp::Animation                boomClip;
    retropp::Animation                thrusterClip;
    std::array<retropp::Animation, 3> bobClips;   // per colonist look (hood / pack / cap)
    retropp::Animation                wingsClip;
    retropp::Animation                pulseClip;
    retropp::Animation                beaconClip;   // palette cycle (beacon_a / beacon_b)
    retropp::Animation                waterClip;    // the SEA's 3-frame roll: tile phase (its
                                                    // index picks each variant's frame) + the
                                                    // water_a/water_b palette breathing over it
    retropp::Animation                lightsClip;   // the livery blink phase (index read only)

    [[nodiscard]] retropp::AtlasId fontAtlas() const { return font.atlasId; }
    [[nodiscard]] retropp::AtlasId terrainAtlas() const { return terrain.atlasId; }
    [[nodiscard]] retropp::AtlasId spriteAtlas() const { return sheet.atlasId; }
    [[nodiscard]] retropp::AtlasId titleAtlas() const { return title.atlasId; }

    // The sheets are 8-wide grids; the slicer's manifest maps slot index → its grid cell, so
    // slot s's atlas cell is simply sheet[s].tile wherever the grid wrapped it.
    [[nodiscard]] std::uint16_t slotTile(Slot s) const {
        return sheet[static_cast<std::size_t>(s)].tile;
    }
    // A terrain tile's top-left engine cell; its 4×4 group offsets by (+dx, +kTerrainStride8·dy).
    [[nodiscard]] std::uint16_t terrainTile(TerrainTile t) const {
        return terrain[static_cast<std::size_t>(t)].tile;
    }
    // A rich glyph's top-left engine tile (digits, A–Z, space for anything unmapped); its 2×2
    // group offsets by (+dx, +kFontStride8·dy) — the stamping helper in render.cpp does the walk.
    [[nodiscard]] std::uint16_t glyphBase(char ch) const {
        std::size_t k = 36;  // space (the 37th cell) for anything unmapped
        if (ch >= '0' && ch <= '9')      k = static_cast<std::size_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'Z') k = static_cast<std::size_t>(10 + (ch - 'A'));
        return font[k].tile;
    }
    // The full-width bevelled rule (the generator appends it as the LAST font cell).
    [[nodiscard]] std::uint16_t ruleBase() const { return font[font.tileCount() - 1].tile; }

    // The livery's palette for a running-light phase (0/1) — the blink is palette selection.
    [[nodiscard]] retropp::PaletteId vehiclePal(int kind, std::size_t phase) const {
        const std::size_t base = static_cast<std::size_t>(PAL_DART_A) +
                                 static_cast<std::size_t>(kind) * 2;
        return spritePals[base + (phase & 1)];
    }
};

// Every sheet is a standard 8-wide GRID (cells wrap onto new rows). Stamp strides derive from
// that fixed width: the font sheet is 8 × 16px = 128px wide → 16 8px-tile columns (the 2×2
// stamp stride); the terrain sheet is 8 × 32px = 256px wide → 32 columns (the 4×4 stride).
constexpr int kFontStride8    = 16;
constexpr int kTerrainStride8 = 32;

// Load + slice all three sheets, load every palette image, build the shared clips. A plain value —
// the clips' frames name their sheets by AtlasId, so the assets object returns, moves, and copies
// freely. May throw (loadAtlas throws on a missing / non-indexed sheet); the caller wraps it.
[[nodiscard]] FerrymanAssets loadFerrymanAssets(retropp::Renderer& renderer);

}  // namespace ferryman
