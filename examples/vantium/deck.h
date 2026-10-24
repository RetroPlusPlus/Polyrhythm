#pragma once

// Vantium — the DECK, its own translation unit: the dreadnought's hull, assembled per ship from
// authored 20×18 section MAP PNGS (bow | five mids | the landing strip | stern) loaded through
// the engine's map-import pipeline — 16-bit grayscale, one pixel per 16px cell, ids spread across
// the whole range so the maps read to a human eye and stay hand-editable (see MapGlyph). The mid
// sequence is chosen by an LCG seeded with the SHIP NUMBER, so ship N always lays out the same
// (learnable) while ships differ from each other.
//
// Assembly produces everything the rest of the game consumes:
//   • the deck layer's engine tiles (each 16×16 art cell stamped as a 2×2 tile group — with
//     orientation coming from FLIPS + 90° ROTATION at stamp time, never extra art: one hazard
//     tile serves all four deck edges, one corner all four corners, one pipe both axes),
//   • a SHADOW pass (a plate directly beneath raised superstructure restamps under the darker
//     ramp palette — depth for free),
//   • the 16px-cell collision grid (raised structure, rotors, vents — the lethal set),
//   • the fuel-pod registry (shootable; a hit restamps the scorched art), mine spawn points,
//     and the landing strip's world rectangle,
//   • the starfield layer's tiles (built once; palette-cycled for the twinkle).

#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"  // TileCell

#include "assets.h"
#include "layout.h"

namespace vant {

struct Pod {
    VCell cell{};       // the top cell; the pod occupies this + the cell below
    bool  alive = true;
};

class Deck {
public:
    // Assemble dreadnought `shipNum` (1-based). Needs the loaded assets for atlas/palette handles.
    void build(int shipNum, const VantAssets& assets);

    // The deck layer's content: 320×36 engine tiles, row-major (kWorldW/8 × kDeckRows*2).
    [[nodiscard]] const std::vector<retropp::TileCell>& tiles() const { return tiles_; }

    // The starfield layer's content: 24×16 engine tiles (a 192×128 map, Repeat-wrapped).
    [[nodiscard]] const std::vector<retropp::TileCell>& starTiles() const { return starTiles_; }
    static constexpr int kStarCols8 = 24, kStarRows8 = 16;
    // Flip every star cell between the two twinkle palettes (called per tick by the renderer's
    // feel-driven pass — palette animation on a whole layer, zero art churn).
    void setStarPalette(retropp::PaletteId pal);
    // Flip the pod glow cells between their two palettes (skips scorched pods).
    void setPodPalette(retropp::PaletteId pal);

    // Collision against the lethal set, world px (false anywhere off the deck band).
    [[nodiscard]] bool rectHitsSolid(float x, float y, float w, float h) const;

    // The shootable pod under a world-px point (-1 if none alive there).
    [[nodiscard]] int podAt(float x, float y) const;
    void scorchPod(int i, const VantAssets& assets);   // restamp the burned art, mark dead
    [[nodiscard]] const std::vector<Pod>& pods() const { return pods_; }

    [[nodiscard]] const std::vector<VCell>& mineSpawns() const { return mineSpawns_; }

    // The landing strip's world rect (x span + the deck-surface y the Manta settles onto).
    [[nodiscard]] float stripX0() const { return stripX0_; }
    [[nodiscard]] float stripX1() const { return stripX1_; }
    [[nodiscard]] float stripY() const { return stripY_; }

private:
    void stampCell16(int c, int r, TileArt art, TilePal pal, bool flipX, bool flipY, bool rot90,
                     const VantAssets& assets);

    std::vector<retropp::TileCell> tiles_;
    std::vector<retropp::TileCell> starTiles_;
    std::vector<std::uint8_t>      solid_;       // kDeckRows × kDeckCols
    std::vector<Pod>               pods_;
    std::vector<std::int16_t>      podIndex_;    // per cell: pod index or -1
    std::vector<VCell>             mineSpawns_;
    std::vector<std::size_t>       starSlots_;   // indices of non-empty star cells (twinkle pass)
    float stripX0_ = 0, stripX1_ = 0, stripY_ = 0;
};

}  // namespace vant
