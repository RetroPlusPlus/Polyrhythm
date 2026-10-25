#include "retropp/draw_state.h"

#include <gtest/gtest.h>

namespace retropp {

// sampleTilemap mirrors the tile fragment shader's per-pixel mapping: an output pixel + the
// layer scroll → the wrapped tile cell + the within-tile offset. tilePx defaults to 8 (the
// GB tile size). A 4×4-tile map is 32×32 px.

TEST(Tilemap, ZeroScrollIdentity) {
    const auto s = sampleTilemap(0, 0, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelX, 0);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, WithinTileOffsetNoBoundaryCross) {
    const auto s = sampleTilemap(3, 5, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelX, 3);
    EXPECT_EQ(s.pixelY, 5);
}

TEST(Tilemap, CrossTileBoundary) {
    const auto s = sampleTilemap(8, 9, LayerScroll{0, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 0);
    EXPECT_EQ(s.tileY, 1);
    EXPECT_EQ(s.pixelY, 1);
}

TEST(Tilemap, PositiveScroll) {
    // world x = 0 + 10 = 10 → tile 1, within-tile pixel 2.
    const auto s = sampleTilemap(0, 0, LayerScroll{10, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 2);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, NegativeScrollWrapsAndOffsetsCorrectly) {
    // world x = -1 → floorDiv(-1,8) = -1 → wrap floorMod(-1,4) = tile 3; pixel floorMod(-1,8) = 7.
    const auto s = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
}

TEST(Tilemap, ScrollOneFullMapWrapsToOrigin) {
    // map width = 4 tiles * 8 px = 32 px; world x = 32 → tile floorMod(4,4) = 0; pixel 0.
    const auto s = sampleTilemap(0, 0, LayerScroll{32, 0}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.pixelX, 0);
}

TEST(Tilemap, VerticalAxisWrapsIndependently) {
    // world y = -8 → tile floorDiv(-8,8) = -1 → floorMod(-1,4) = 3; pixel 0. X axis untouched.
    const auto s = sampleTilemap(0, 0, LayerScroll{0, -8}, 4, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.tileY, 3);
    EXPECT_EQ(s.pixelY, 0);
}

TEST(Tilemap, NonSquareMapWrapsPerAxisDimension) {
    // 2 tiles wide (16 px), 4 tiles tall (32 px). world x = 17 → tile floorMod(2,2)=0, pixel 1.
    const auto s = sampleTilemap(1, 0, LayerScroll{16, 0}, 2, 4);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.pixelX, 1);
}

TEST(Tilemap, ConstexprEvaluable) {
    // The mapping is usable in constant evaluation (it must match the shader exactly).
    constexpr auto s = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4);
    static_assert(s.tileX == 3, "negative-scroll wrap must be constexpr-correct");
    static_assert(s.pixelX == 7);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
}

// ── Per-layer tilemap wrap mode (Repeat / Clamp / Blank) ─────────────────────
// A 4×4-tile map is 32×32 px; the last in-range pixel is 31 on each axis. Repeat is the default
// and must reproduce the toroidal behavior above; Clamp pins out-of-range to the edge; Blank
// flags out-of-range as a finite-map hole the shader discards.

TEST(TilemapWrap, RepeatExplicitMatchesDefault) {
    // Passing Repeat explicitly == the defaulted (pre) toroidal mapping, byte-for-byte.
    const auto def  = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4);
    const auto repl = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4, TileWrap::Repeat);
    EXPECT_EQ(repl.tileX, def.tileX);
    EXPECT_EQ(repl.pixelX, def.pixelX);
    EXPECT_EQ(repl.tileX, 3);
    EXPECT_EQ(repl.pixelX, 7);
    EXPECT_FALSE(repl.outside);
}

TEST(TilemapWrap, ClampInRangeIsIdentity) {
    // In-range world coords are untouched by Clamp.
    const auto s = sampleTilemap(10, 5, LayerScroll{0, 0}, 4, 4, TileWrap::Clamp);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 2);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelY, 5);
    EXPECT_FALSE(s.outside);
}

TEST(TilemapWrap, ClampPinsPositiveOverflowToEdgeTile) {
    // world x = 40 > 31 → clamp to 31 → last tile (3), last within-tile pixel (7). NOT a wrap to 0.
    const auto s = sampleTilemap(0, 0, LayerScroll{40, 0}, 4, 4, TileWrap::Clamp);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
    EXPECT_FALSE(s.outside);
}

TEST(TilemapWrap, ClampPinsNegativeToZero) {
    // world x = -5 → clamp to 0 → tile 0, pixel 0. NOT a wrap to the far edge.
    const auto s = sampleTilemap(0, 0, LayerScroll{-5, 0}, 4, 4, TileWrap::Clamp);
    EXPECT_EQ(s.tileX, 0);
    EXPECT_EQ(s.pixelX, 0);
    EXPECT_FALSE(s.outside);
}

TEST(TilemapWrap, BlankInRangeSamplesNormally) {
    const auto s = sampleTilemap(10, 5, LayerScroll{0, 0}, 4, 4, TileWrap::Blank);
    EXPECT_EQ(s.tileX, 1);
    EXPECT_EQ(s.pixelX, 2);
    EXPECT_EQ(s.tileY, 0);
    EXPECT_EQ(s.pixelY, 5);
    EXPECT_FALSE(s.outside);
}

TEST(TilemapWrap, BlankLastInRangePixelIsInside) {
    // world x = 31 is the last in-range pixel → still inside (tile 3, pixel 7), not a hole.
    const auto s = sampleTilemap(0, 0, LayerScroll{31, 0}, 4, 4, TileWrap::Blank);
    EXPECT_EQ(s.tileX, 3);
    EXPECT_EQ(s.pixelX, 7);
    EXPECT_FALSE(s.outside);
}

TEST(TilemapWrap, BlankOutOfRangePositiveIsHole) {
    // world x = 32 == mapPx → outside [0, 32) → finite-map hole. Under Repeat this would wrap to
    // the origin (see ScrollOneFullMapWrapsToOrigin); the distinct Blank behavior is the point.
    const auto s = sampleTilemap(0, 0, LayerScroll{32, 0}, 4, 4, TileWrap::Blank);
    EXPECT_TRUE(s.outside);
}

TEST(TilemapWrap, BlankOutOfRangeNegativeIsHole) {
    const auto s = sampleTilemap(0, 0, LayerScroll{-1, 0}, 4, 4, TileWrap::Blank);
    EXPECT_TRUE(s.outside);
}

TEST(TilemapWrap, BlankVerticalAxisIndependentlyHoles) {
    // X in range, Y out of range → still a hole (a coord outside EITHER axis is a hole).
    const auto s = sampleTilemap(0, 0, LayerScroll{0, 32}, 4, 4, TileWrap::Blank);
    EXPECT_TRUE(s.outside);
}

TEST(TilemapWrap, ConstexprBlankHoleAndInside) {
    // Both the hole flag and the in-range sample are constant-evaluable (they must match the shader).
    constexpr auto hole = sampleTilemap(0, 0, LayerScroll{32, 0}, 4, 4, TileWrap::Blank);
    static_assert(hole.outside, "Blank out-of-range must constexpr-flag a hole");
    constexpr auto in = sampleTilemap(0, 0, LayerScroll{31, 0}, 4, 4, TileWrap::Blank);
    static_assert(!in.outside && in.tileX == 3 && in.pixelX == 7);
    EXPECT_TRUE(hole.outside);
    EXPECT_FALSE(in.outside);
}

}  // namespace retropp
