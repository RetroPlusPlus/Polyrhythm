#include "retropp/draw_state.h"
#include "retropp/image.h"     // AtlasId
#include "retropp/palette.h"   // PaletteId

#include <cstdint>
#include <set>
#include <utility>

#include <gtest/gtest.h>

namespace retropp {

// The R32G32_UINT tilemap cell the tile shader unpacks:
//   word0: tile (0..15) | flipX (16) | flipY (17) | rotation (18..19)
//   word1: atlas (0..15, AtlasId) | palette (16..31, PaletteId)
// packTileCell / unpackTileCell are the constexpr mirror of that GPU layout; the shader unpacks the
// identical bits. Each cell names its own sheet + palette directly — no per-layer set or select.

TEST(TileCell, PackLayoutHasLockedBitPositions) {
    // tile in word0 low 16.
    EXPECT_EQ(packTileCell(TileCell{.tile = 0x1234}), (PackedTileCell{0x00001234u, 0u}));
    // flipX is word0 bit 16, flipY is word0 bit 17.
    EXPECT_EQ(packTileCell(TileCell{.flipX = true}), (PackedTileCell{0x00010000u, 0u}));
    EXPECT_EQ(packTileCell(TileCell{.flipY = true}), (PackedTileCell{0x00020000u, 0u}));
    // atlas in word1 low 16.
    EXPECT_EQ(packTileCell(TileCell{.atlas = static_cast<AtlasId>(0xABCD)}),
              (PackedTileCell{0u, 0x0000ABCDu}));
    // palette in word1 high 16.
    EXPECT_EQ(packTileCell(TileCell{.palette = static_cast<PaletteId>(0x1357)}),
              (PackedTileCell{0u, 0x13570000u}));
    // rotation is word0 bits 18..19 (None=0, Rot90=1, Rot180=2, Rot270=3).
    EXPECT_EQ(packTileCell(TileCell{.rotation = Rotation::Rot90}),  (PackedTileCell{0x00040000u, 0u}));
    EXPECT_EQ(packTileCell(TileCell{.rotation = Rotation::Rot180}), (PackedTileCell{0x00080000u, 0u}));
    EXPECT_EQ(packTileCell(TileCell{.rotation = Rotation::Rot270}), (PackedTileCell{0x000C0000u, 0u}));
    // combined (every field at full width, all flips + max rotation → word0 bits 0..19 set).
    EXPECT_EQ(packTileCell(TileCell{.atlas = static_cast<AtlasId>(0xFFFF), .tile = 0xFFFF,
                                    .palette = static_cast<PaletteId>(0xFFFF),
                                    .flipX = true, .flipY = true, .rotation = Rotation::Rot270}),
              (PackedTileCell{0x000FFFFFu, 0xFFFFFFFFu}));
}

TEST(TileCell, UnpackReadsEachField) {
    const TileCell c = unpackTileCell(PackedTileCell{0x0003CDEFu, 0xABCD1357u});
    EXPECT_EQ(c.tile, 0xCDEF);
    EXPECT_TRUE(c.flipX);
    EXPECT_TRUE(c.flipY);
    EXPECT_EQ(c.atlas, static_cast<AtlasId>(0x1357));     // word1 low 16
    EXPECT_EQ(c.palette, static_cast<PaletteId>(0xABCD)); // word1 high 16
}

TEST(TileCell, PackUnpackRoundTripsAcrossFieldRanges) {
    for (std::uint32_t tile : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x7FFF},
                              std::uint32_t{0x8000}, std::uint32_t{0xFFFF}}) {
        for (std::uint32_t atlas : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x1234},
                                   std::uint32_t{0xFFFF}}) {
            for (std::uint32_t pal : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x80},
                                     std::uint32_t{0xFFFF}}) {
                for (int fx = 0; fx < 2; ++fx) {
                    for (int fy = 0; fy < 2; ++fy) {
                        for (Rotation rot : {Rotation::None, Rotation::Rot90,
                                             Rotation::Rot180, Rotation::Rot270}) {
                            const TileCell in{.atlas   = static_cast<AtlasId>(atlas),
                                              .tile    = static_cast<std::uint16_t>(tile),
                                              .palette = static_cast<PaletteId>(pal),
                                              .flipX   = fx != 0,
                                              .flipY   = fy != 0,
                                              .rotation = rot};
                            const TileCell out = unpackTileCell(packTileCell(in));
                            EXPECT_EQ(out.tile, in.tile);
                            EXPECT_EQ(out.atlas, in.atlas);
                            EXPECT_EQ(out.palette, in.palette);
                            EXPECT_EQ(out.flipX, in.flipX);
                            EXPECT_EQ(out.flipY, in.flipY);
                            EXPECT_EQ(out.rotation, in.rotation);
                        }
                    }
                }
            }
        }
    }
}

TEST(TileCell, PackIsConstexpr) {
    constexpr PackedTileCell packed = packTileCell(
        TileCell{.atlas = static_cast<AtlasId>(0x2A), .tile = 0x00C8,
                 .palette = static_cast<PaletteId>(0x05), .flipY = true});
    static_assert((packed.w0 & 0xFFFF) == 0x00C8, "tile bits");
    static_assert(((packed.w0 >> 17) & 1) == 1, "flipY bit");
    static_assert((packed.w1 & 0xFFFF) == 0x2A, "atlas bits");
    static_assert((packed.w1 >> 16) == 0x05, "palette bits");
    EXPECT_EQ(unpackTileCell(packed).tile, 0x00C8);
}

// sourceCellTexel maps a destination within-cell pixel to the source atlas pixel, applying flips then
// the 90° rotation. The tile and sprite fragment shaders reproduce it exactly. Order is "rotate the
// art, then mirror" (flip the destination coord first, then inverse-rotate).

TEST(SourceCellTexel, IdentityReturnsInput) {
    EXPECT_EQ(sourceCellTexel(3, 5, 8, 8, Rotation::None, false, false), (CellTexel{3, 5}));
}

TEST(SourceCellTexel, FlipsAloneMatchTheClassicMirror) {
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::None, true, false),  (CellTexel{6, 0}));  // flipX
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::None, false, true),  (CellTexel{1, 7}));  // flipY
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::None, true, true),   (CellTexel{6, 7}));  // both
}

TEST(SourceCellTexel, FourRotationsOnAnAsymmetricPixel) {
    // The near-corner pixel (1,0) on an 8×8 cell (n = 7), clockwise.
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::None,   false, false), (CellTexel{1, 0}));
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::Rot90,  false, false), (CellTexel{0, 6}));
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::Rot180, false, false), (CellTexel{6, 7}));
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::Rot270, false, false), (CellTexel{7, 1}));
}

TEST(SourceCellTexel, RotateThenMirrorOrder) {
    // flipX is applied to the destination coord BEFORE the rotation: (1,0) flipX→(6,0), then Rot90→(0,1).
    EXPECT_EQ(sourceCellTexel(1, 0, 8, 8, Rotation::Rot90, true, false), (CellTexel{0, 1}));
}

TEST(SourceCellTexel, EightOrientationsAreDistinct) {
    // The dihedral group D4: four rotations × {no flip, flipX} give the eight distinct orientations of
    // square art. An asymmetric interior pixel must land in eight different places.
    std::set<std::pair<int, int>> seen;
    for (Rotation rot : {Rotation::None, Rotation::Rot90, Rotation::Rot180, Rotation::Rot270}) {
        for (bool fx : {false, true}) {
            const CellTexel t = sourceCellTexel(1, 2, 8, 8, rot, fx, false);
            seen.insert({t.x, t.y});
        }
    }
    EXPECT_EQ(seen.size(), 8u);
}

TEST(SourceCellTexel, NonSquareRot90Transposes) {
    // An 8-wide × 16-tall sprite: Rot90 swaps the read extents (x now spans [0,16), y [0,8)). The
    // bottom-left destination pixel (0,15) reads source (15,7) — the transposed corner.
    EXPECT_EQ(sourceCellTexel(0, 15, 8, 16, Rotation::Rot90, false, false), (CellTexel{15, 7}));
    // Rot180 is dimension-safe at any size.
    EXPECT_EQ(sourceCellTexel(0, 0, 8, 16, Rotation::Rot180, false, false), (CellTexel{7, 15}));
}

TEST(SourceCellTexel, IsConstexpr) {
    static_assert(sourceCellTexel(0, 0, 8, 8, Rotation::Rot90, false, false) == CellTexel{0, 7});
    static_assert(sourceCellTexel(0, 0, 8, 8, Rotation::None, false, false) == CellTexel{0, 0});
    SUCCEED();
}

// paletteStoreTexel mirrors the shaders' flat palette lookup: (offset + index) wraps to (flat % W,
// flat / W) in the W-wide flat store. A palette may straddle rows; capacity is W × height, not 256.

TEST(PaletteStoreTexel, MapsFlatPositionIntoTheWideStore) {
    // Within the first row.
    EXPECT_EQ(paletteStoreTexel(0u, 5u, 16384u), (PaletteTexel{5u, 0u}));
    EXPECT_EQ(paletteStoreTexel(300u, 7u, 16384u), (PaletteTexel{307u, 0u}));
}

TEST(PaletteStoreTexel, WrapsAcrossRows) {
    // W = 256 here (a small store) to exercise the row wrap cheaply. flat 256 → (0,1), 257 → (1,1).
    EXPECT_EQ(paletteStoreTexel(250u, 6u, 256u), (PaletteTexel{0u, 1u}));
    EXPECT_EQ(paletteStoreTexel(250u, 7u, 256u), (PaletteTexel{1u, 1u}));
    // A palette straddling a row boundary: offset 254, indices 0..3 cross from row 0 into row 1.
    EXPECT_EQ(paletteStoreTexel(254u, 1u, 256u), (PaletteTexel{255u, 0u}));
    EXPECT_EQ(paletteStoreTexel(254u, 2u, 256u), (PaletteTexel{0u, 1u}));
}

TEST(PaletteStoreTexel, AddressesArbitraryLargePalettes) {
    // A color index of 65535 into a palette at offset 200000, store width 16384.
    const std::uint32_t flat = 200000u + 65535u;  // 265535
    EXPECT_EQ(paletteStoreTexel(200000u, 65535u, 16384u),
              (PaletteTexel{flat % 16384u, flat / 16384u}));
}

TEST(PaletteStoreTexel, IsConstexpr) {
    static_assert(paletteStoreTexel(0u, 0u, 16384u) == PaletteTexel{0u, 0u});
    static_assert(paletteStoreTexel(16384u, 0u, 16384u) == PaletteTexel{0u, 1u});
    SUCCEED();
}

}  // namespace retropp
