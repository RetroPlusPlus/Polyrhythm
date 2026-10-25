#include <gtest/gtest.h>

#include "retropp/geometry.h"

namespace retropp {
namespace {

constexpr PixelSize kGb{160, 144};

TEST(Geometry, ExactIntegerMultipleFillsWithNoBars) {
    // 4× the GB viewport exactly: full coverage, origin at (0,0), no letterbox.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{640, 576}, kGb), (IntRect{0, 0, 640, 576}));
}

TEST(Geometry, OneToOneWhenDrawableEqualsViewport) {
    EXPECT_EQ(integerScaleToFitRect(kGb, kGb), (IntRect{0, 0, 160, 144}));
}

TEST(Geometry, NonExactPicksLargestFittingMultipleAndCentres) {
    // 700×600 fits 4× (640×576); remainder split as symmetric bars (30 px / 12 px).
    EXPECT_EQ(integerScaleToFitRect(PixelSize{700, 600}, kGb), (IntRect{30, 12, 640, 576}));
}

TEST(Geometry, BindingDimensionLimitsTheScale) {
    // Width allows 2× (320/160), height allows 4× (600/144) → scale is the min, 2×.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{320, 600}, kGb), (IntRect{0, 156, 320, 288}));
}

TEST(Geometry, SmallerThanViewportClampsToOneXAndOverflows) {
    // Below 1× the content is shown at 1× and the centered rect goes negative rather
    // than collapsing — the window is simply smaller than a single GB frame.
    EXPECT_EQ(integerScaleToFitRect(PixelSize{100, 100}, kGb), (IntRect{-30, -22, 160, 144}));
}

TEST(Geometry, DegenerateSizesYieldEmptyRect) {
    EXPECT_EQ(integerScaleToFitRect(PixelSize{0, 0}, kGb), (IntRect{}));
    EXPECT_EQ(integerScaleToFitRect(PixelSize{640, 576}, PixelSize{0, 144}), (IntRect{}));
    EXPECT_EQ(integerScaleToFitRect(PixelSize{-10, -10}, kGb), (IntRect{}));
}

// Compile-time confirmation the helper is usable in constant expressions.
static_assert(integerScaleToFitRect(PixelSize{640, 576}, kGb) == IntRect{0, 0, 640, 576});

// ── fitWindowScale: target window scale clamped to the usable display ──

TEST(Geometry, FitWindowScaleUsesTargetWhenItFits) {
    // 4× the GB viewport (640×576) fits comfortably in a 2560×1440 desktop → the target stands.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{2560, 1440}, 4), 4);
}

TEST(Geometry, FitWindowScaleClampsDownByHeight) {
    // 4× height (576) exceeds a 500-tall usable area → step down to the largest that fits: 3× (432).
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{2560, 500}, 4), 3);
}

TEST(Geometry, FitWindowScaleClampsDownByWidth) {
    // 4× width (640) exceeds a 500-wide usable area → 3× (480) fits.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{500, 1440}, 4), 3);
}

TEST(Geometry, FitWindowScaleFloorsAtOneForHugeViewport) {
    // A viewport bigger than the whole usable display still yields 1× (never 0); the window opens at
    // the OS limit and the content letterboxes.
    EXPECT_EQ(fitWindowScale(PixelSize{3000, 2000}, PixelSize{2560, 1440}, 4), 1);
}

TEST(Geometry, FitWindowScaleDegenerateReturnsTarget) {
    // Unknown display (non-positive usable) can't clamp → the target (min 1) passes through.
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{0, 0}, 4), 4);
    EXPECT_EQ(fitWindowScale(kGb, PixelSize{0, 0}, 0), 1);  // floor at 1×
}

// Compile-time confirmation the clamp is a constant expression.
static_assert(fitWindowScale(kGb, PixelSize{2560, 1440}, 4) == 4);
static_assert(fitWindowScale(kGb, PixelSize{2560, 500}, 4) == 3);

// ── windowToViewport: invert the blit to map a window pixel into viewport space (pointer/analog) ──

TEST(Geometry, WindowToViewportFullCoverageOneToOne) {
    // 1× blit filling the window: a window pixel maps to the same viewport pixel.
    const IntRect blit{0, 0, 160, 144};
    EXPECT_EQ(windowToViewport(Vec2i{0, 0}, blit, kGb), (ViewportHit{Vec2i{0, 0}, true}));
    EXPECT_EQ(windowToViewport(Vec2i{159, 143}, blit, kGb), (ViewportHit{Vec2i{159, 143}, true}));
}

TEST(Geometry, WindowToViewportDividesByIntegerScale) {
    // 4× blit at the origin: a window pixel maps to ⌊pixel / 4⌋ in viewport space.
    const IntRect blit{0, 0, 640, 576};
    EXPECT_EQ(windowToViewport(Vec2i{0, 0}, blit, kGb).pos, (Vec2i{0, 0}));
    EXPECT_EQ(windowToViewport(Vec2i{7, 11}, blit, kGb).pos, (Vec2i{1, 2}));   // 7/4=1, 11/4=2
    EXPECT_EQ(windowToViewport(Vec2i{639, 575}, blit, kGb).pos, (Vec2i{159, 143}));
    EXPECT_TRUE(windowToViewport(Vec2i{320, 288}, blit, kGb).inside);
}

TEST(Geometry, WindowToViewportSubtractsTheLetterboxOrigin) {
    // 4× blit centered in a 700×600 window (origin 30,12 — the geometry from integerScaleToFitRect):
    // a window pixel at the blit origin maps to viewport (0,0); the center maps inside.
    const IntRect blit = integerScaleToFitRect(PixelSize{700, 600}, kGb);  // {30, 12, 640, 576}
    EXPECT_EQ(blit, (IntRect{30, 12, 640, 576}));
    EXPECT_EQ(windowToViewport(Vec2i{30, 12}, blit, kGb), (ViewportHit{Vec2i{0, 0}, true}));
    EXPECT_EQ(windowToViewport(Vec2i{30 + 8, 12 + 4}, blit, kGb).pos, (Vec2i{2, 1}));  // 8/4, 4/4
}

TEST(Geometry, WindowToViewportFlagsOffContentInTheLetterbox) {
    // A pixel in the left pillarbox (x < 30) and one past the right edge are both off-content.
    const IntRect blit = integerScaleToFitRect(PixelSize{700, 600}, kGb);  // {30, 12, 640, 576}
    EXPECT_FALSE(windowToViewport(Vec2i{10, 300}, blit, kGb).inside);   // left of content
    EXPECT_FALSE(windowToViewport(Vec2i{30 + 640, 300}, blit, kGb).inside);  // just past the right edge
    EXPECT_FALSE(windowToViewport(Vec2i{300, 5}, blit, kGb).inside);    // above content (y < 12)
    // The returned coordinate is still clamped into the viewport so a consumer can read it safely.
    const ViewportHit hit = windowToViewport(Vec2i{0, 0}, blit, kGb);
    EXPECT_FALSE(hit.inside);
    EXPECT_EQ(hit.pos, (Vec2i{0, 0}));
}

TEST(Geometry, WindowToViewportDegenerateInputsYieldFalseHit) {
    EXPECT_EQ(windowToViewport(Vec2i{5, 5}, IntRect{}, kGb), (ViewportHit{}));
    EXPECT_EQ(windowToViewport(Vec2i{5, 5}, IntRect{0, 0, 640, 576}, PixelSize{0, 144}),
              (ViewportHit{}));
}

// Compile-time confirmation the inverse map is a constant expression too.
static_assert(windowToViewport(Vec2i{7, 11}, IntRect{0, 0, 640, 576}, kGb).pos == Vec2i{1, 2});
static_assert(windowToViewport(Vec2i{0, 0}, IntRect{30, 12, 640, 576}, kGb).inside == false);

// ── composeScaleToFit — the output-resolution compose grid factor ────────────────────────

TEST(Geometry, ComposeScaleEqualsTheIntegerFitFactor) {
    // The compose scale is the window's integer-scale-to-fit multiple — compose at the drawn-region
    // size so the blit is a 1:1 centering copy (fill parity with the faithful path).
    EXPECT_EQ(composeScaleToFit(PixelSize{640, 576}, kGb, 16), 4);   // exactly 4×
    EXPECT_EQ(composeScaleToFit(PixelSize{700, 600}, kGb, 16), 4);   // 4× fits, remainder letterboxes
    EXPECT_EQ(composeScaleToFit(kGb, kGb, 16), 1);                   // 1:1 window
}

TEST(Geometry, ComposeScaleClampsToTheCap) {
    // A 4K drawable would fit 15× (min(3840/160, 2160/144)); the cap holds it at the maximum.
    EXPECT_EQ(composeScaleToFit(PixelSize{3840, 2160}, kGb, 16), 15);  // under the cap → the real factor
    EXPECT_EQ(composeScaleToFit(PixelSize{3840, 2160}, kGb, 6), 6);    // capped below the fit factor
    EXPECT_EQ(composeScaleToFit(PixelSize{6400, 5760}, kGb, 16), 16);  // 40× fit, clamped to the cap
}

TEST(Geometry, ComposeScaleFloorsAtOne) {
    // A window smaller than the viewport still composes at 1× (never zero) — the blit shows 1× content.
    EXPECT_EQ(composeScaleToFit(PixelSize{80, 72}, kGb, 16), 1);
    EXPECT_EQ(composeScaleToFit(PixelSize{0, 0}, kGb, 16), 1);        // degenerate → 1
    EXPECT_EQ(composeScaleToFit(PixelSize{640, 576}, PixelSize{0, 144}, 16), 1);  // degenerate viewport → 1
    EXPECT_EQ(composeScaleToFit(PixelSize{640, 576}, kGb, 0), 1);     // maxScale floored at 1
}

TEST(Geometry, ComposeScaleIsConstexpr) {
    static_assert(composeScaleToFit(PixelSize{640, 576}, kGb, 16) == 4);
    static_assert(composeScaleToFit(PixelSize{3840, 2160}, kGb, 6) == 6);
    SUCCEED();
}

}  // namespace
}  // namespace retropp
