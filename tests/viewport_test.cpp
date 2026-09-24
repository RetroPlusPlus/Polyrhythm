#include <gtest/gtest.h>

#include "retropp/viewport.h"

namespace retropp {
namespace {

TEST(Viewport, DefaultsToGameBoyResolution) {
    constexpr ViewportResolution config{};
    EXPECT_EQ(config.width, 160);
    EXPECT_EQ(config.height, 144);
}

TEST(Viewport, OverrideIsReportedBack) {
    constexpr ViewportResolution wide{320, 144};
    EXPECT_EQ(wide.width, 320);
    EXPECT_EQ(wide.height, 144);
}

// The named presets are just {width, height} tuples for common platforms.

TEST(Viewport, PresetsHoldExpectedDimensions) {
    EXPECT_EQ(ViewportResolution::GameBoy.width, 160);
    EXPECT_EQ(ViewportResolution::GameBoy.height, 144);
    EXPECT_EQ(ViewportResolution::GameBoyAdvance.width, 240);
    EXPECT_EQ(ViewportResolution::GameBoyAdvance.height, 160);
    EXPECT_EQ(ViewportResolution::Nes.width, 256);
    EXPECT_EQ(ViewportResolution::Nes.height, 240);
    EXPECT_EQ(ViewportResolution::Snes.width, 256);
    EXPECT_EQ(ViewportResolution::Snes.height, 224);
    EXPECT_EQ(ViewportResolution::SnesHiRes.width, 512);
    EXPECT_EQ(ViewportResolution::SnesHiRes.height, 224);
    EXPECT_EQ(ViewportResolution::SnesInterlaced.width, 256);
    EXPECT_EQ(ViewportResolution::SnesInterlaced.height, 448);
    EXPECT_EQ(ViewportResolution::SnesHiResInterlaced.width, 512);
    EXPECT_EQ(ViewportResolution::SnesHiResInterlaced.height, 448);
    EXPECT_EQ(ViewportResolution::Genesis.width, 320);
    EXPECT_EQ(ViewportResolution::Genesis.height, 224);
    EXPECT_EQ(ViewportResolution::MasterSystem.width, 256);
    EXPECT_EQ(ViewportResolution::MasterSystem.height, 192);
}

TEST(Viewport, GameBoyAndColorAreSameResolutionDistinctNames) {
    EXPECT_EQ(ViewportResolution::GameBoyColor.width, ViewportResolution::GameBoy.width);
    EXPECT_EQ(ViewportResolution::GameBoyColor.height, ViewportResolution::GameBoy.height);
}

TEST(Viewport, PresetIsInterchangeableWithRawConfig) {
    // A preset is usable wherever a raw ViewportResolution is — same type, no conversion.
    constexpr ViewportResolution fromPreset = ViewportResolution::Snes;
    constexpr ViewportResolution raw{256, 224};
    EXPECT_EQ(fromPreset.width, raw.width);
    EXPECT_EQ(fromPreset.height, raw.height);
}

TEST(Viewport, DefaultStillMatchesGameBoyPreset) {
    // The ViewportResolution{} default is unchanged — every existing call site keeps GB.
    constexpr ViewportResolution def{};
    EXPECT_EQ(def.width, ViewportResolution::GameBoy.width);
    EXPECT_EQ(def.height, ViewportResolution::GameBoy.height);
}

// composeDimensions scales the viewport into the raster grid the renderer composites onto.

TEST(Viewport, ComposeGridAtScaleOneEqualsViewport) {
    // Scale 1 makes the compose grid identical to the viewport — the invariant the compose path rests on.
    constexpr PixelSize grid = composeDimensions(ViewportResolution::GameBoy, 1);
    EXPECT_EQ(grid.width, 160);
    EXPECT_EQ(grid.height, 144);
}

TEST(Viewport, ComposeGridScalesBothAxes) {
    constexpr PixelSize grid = composeDimensions(ViewportResolution::GameBoy, 3);
    EXPECT_EQ(grid.width, 480);
    EXPECT_EQ(grid.height, 432);
}

TEST(Viewport, ComposeGridScalesNonSquareViewport) {
    // A non-square viewport scales each axis independently by the same factor.
    constexpr PixelSize grid = composeDimensions(ViewportResolution::GameBoyAdvance, 2);
    EXPECT_EQ(grid.width, 480);
    EXPECT_EQ(grid.height, 320);
}

// size() reports the dimensions as a PixelSize; center() gives the geometric middle in viewport pixels.

TEST(Viewport, SizeReportsDimensionsAsPixelSize) {
    constexpr PixelSize s = ViewportResolution{240, 160}.size();
    EXPECT_EQ(s.width, 240);
    EXPECT_EQ(s.height, 160);
}

TEST(Viewport, CenterOfEvenViewportIsWholePixel) {
    constexpr Vec2 c = ViewportResolution::GameBoy.center();  // 160×144
    EXPECT_FLOAT_EQ(c.x, 80.0f);
    EXPECT_FLOAT_EQ(c.y, 72.0f);
}

TEST(Viewport, CenterOfOddViewportIsHalfPixel) {
    // An arbitrary (odd) viewport centers on a half-pixel — the case the float center exists for.
    constexpr Vec2 c = ViewportResolution{257, 145}.center();
    EXPECT_FLOAT_EQ(c.x, 128.5f);
    EXPECT_FLOAT_EQ(c.y, 72.5f);
}

}  // namespace
}  // namespace retropp
