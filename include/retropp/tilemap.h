#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "retropp/draw_state.h"  // TileCell, AtlasId
#include "retropp/image.h"       // IndexGrid, AtlasId
#include "retropp/palette.h"     // PaletteId

namespace retropp {

// ── Tilemap image import: a map IndexGrid + a TileCatalog → a tile layer's cells ─────────────────
//
// A TileCatalog is a plain declared registry of distinct tiles a map can place. Each entry names a
// SHEET (an AtlasId — any loaded atlas), a SLOT within it (the 8px atlas cell index, i.e. an
// AssetSlot::tile), the PALETTE that colors it, a flip, and a rotation. Catalog ENTRY INDEX i is the raw value
// a map pixel holds: a 16-bit grayscale map PNG decodes (loadMapPng → IndexGrid) to a grid of these
// indices, and assembleTilemap() turns that grid into the layer's TileCell array.
//
// The whole point: the catalog spans MANY sheets, and ONE map mixes tiles from several of them —
// each emitted cell names its own sheet and palette directly (no per-layer set, no cap), so a font
// sheet and a menu sheet (say) render together in a single tile layer. No tile is ever repeated that a
// flip or rotation can produce: an entry reuses a sheet slot with flipX/flipY/rotation to get any of
// the eight orientations of square art (one corner tile serves all four corners).
struct TileCatalogEntry {
    std::uint16_t id     = 0;       // the map VALUE that selects this tile — identity, first field. Ids
                                    // are arbitrary 16-bit values (SPARSE: a map can spread them across
                                    // the whole 0..65535 range, which is exactly what exercises a 16-bit
                                    // map — values above 255 an 8-bit decode would misread).
    AtlasId       sheet{};          // which loaded atlas this tile comes from
    std::uint16_t slot   = 0;       // the 8px cell index within that sheet (an AssetSlot::tile)
    PaletteId     palette{};        // which uploaded palette colors this tile
    bool          flipX  = false;
    bool          flipY  = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};

// The catalog: a map VALUE selects the entry whose `id` matches it (NOT positional — ids are sparse).
// A plain declared object — build it inline with designated initializers.
struct TileCatalog {
    std::vector<TileCatalogEntry> entries;
};

// The result of assembling a map: the layer's TileCell array, each cell naming its own sheet + palette
// directly. Owns its vector; a TileContent points its `cells` span at it, so an AssembledTilemap must
// outlive the renderFrame() that consumes the TileContent (the usual game-owned-data lifetime).
struct AssembledTilemap {
    std::vector<TileCell> cells;          // row-major widthInTiles * heightInTiles
    int                   widthInTiles  = 0;
    int                   heightInTiles = 0;

    // Optional convenience: a TileContent wired to display this build — fills cells + dimensions in
    // one call instead of threading each by hand. `wrap` is the one display choice the build doesn't
    // carry. The returned span points INTO this AssembledTilemap, so it must outlive the TileContent
    // (same game-owned-data lifetime as the fields). The manual cells path on TileContent stays
    // first-class — set it directly when a layer mutates its tilemap on the fly.
    [[nodiscard]] TileContent asTileContent(TileWrap wrap = TileWrap::Repeat) const {
        return TileContent{
            .widthInTiles  = widthInTiles,
            .heightInTiles = heightInTiles,
            .cells         = cells,
            .wrap          = wrap,
        };
    }
};

// Assemble a map IndexGrid into a tile layer's data via a TileCatalog. Each grid value selects the
// entry whose `id` matches (sparse 16-bit ids); the entry's sheet, palette, slot, flip, and rotation
// ride directly onto the emitted cell. Throws std::out_of_range if a grid value matches no catalog id and
// std::invalid_argument on a duplicate catalog id. A degenerate (empty) grid yields an empty result.
[[nodiscard]] AssembledTilemap assembleTilemap(const IndexGrid& map, const TileCatalog& catalog);

// Build a run of cells that all draw from one sheet + palette: each `slot` becomes a TileCell with
// `atlas`/`palette` filled and no flip. The single convenience over hand-writing TileCell literals for
// the common single-combo run; the result is plain mutable data — set flips or other handles directly
// for anything that varies. Returns one cell per slot, in order.
[[nodiscard]] std::vector<TileCell> tiles(AtlasId atlas, PaletteId palette,
                                          std::initializer_list<std::uint16_t> slots);

}  // namespace retropp
