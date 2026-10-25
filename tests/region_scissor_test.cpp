#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// regionScissorRect (postprocess.h): the compose-grid bounding box a region-confined effect's runEffect
// pass is scissored to. Pure CPU, device-free — the failable unit behind the renderer's perf fix. The
// bbox must cover every compose pixel the region gate can keep (byte-identical output), and shapes that
// cannot be bounded tight must fall back to the full compose rect ("no scissor"). Margin spec (LOCKED in
// the PLAN A1): inflate by radius + strokeWidth/2, floor/ceil to whole viewport px, ±1 px outward, scale
// by composeScale, clamp to the target; a fully-offscreen shape collapses to a 1×1 valid scissor.

namespace retropp {
namespace {

constexpr int kW = 160;  // viewport 160×144 → compose grid at scale 1
constexpr int kH = 144;

// ── Tight bounding boxes ────────────────────────────────────────────────────────────────

TEST(RegionScissor, CircleBoundingBox) {
    // center (80,72), r 30 → viewport box [50,110]×[42,102]; floor/ceil ±1 → [49,111]×[41,103].
    const IntRect r = regionScissorRect(ShapePoints::circle({80, 72}, 30), 1, kW, kH);
    EXPECT_EQ(r, (IntRect{49, 41, 62, 62}));
}

TEST(RegionScissor, CapsuleBoundingBox) {
    // spine (20,20)→(60,20), r 5 → box [15,65]×[15,25]; floor/ceil ±1 → [14,66]×[14,26].
    const IntRect r = regionScissorRect(ShapePoints::capsule({20, 20}, {60, 20}, 5), 1, kW, kH);
    EXPECT_EQ(r, (IntRect{14, 14, 52, 12}));
}

TEST(RegionScissor, PolygonBoundingBox) {
    // triangle spans x[10,50] y[10,40], radius 0 → floor/ceil ±1 → [9,51]×[9,41].
    const IntRect r = regionScissorRect(ShapePoints::triangle({10, 10}, {50, 10}, {10, 40}), 1, kW, kH);
    EXPECT_EQ(r, (IntRect{9, 9, 42, 32}));
}

TEST(RegionScissor, RadiusInflatesBox) {
    const IntRect small = regionScissorRect(ShapePoints::circle({80, 72}, 10), 1, kW, kH);
    const IntRect big   = regionScissorRect(ShapePoints::circle({80, 72}, 30), 1, kW, kH);
    EXPECT_EQ(small, (IntRect{69, 61, 22, 22}));  // [70,90]×[62,82] → floor/ceil ±1
    EXPECT_GT(big.width, small.width);            // a larger radius widens the box
    EXPECT_GT(big.height, small.height);
}

TEST(RegionScissor, StrokeWidensBox) {
    ShapePoints stroked  = ShapePoints::circle({80, 72}, 20);
    stroked.strokeWidth  = 8;  // inflate = 20 + 4 = 24 → box [56,104]×[48,96]
    const IntRect r      = regionScissorRect(stroked, 1, kW, kH);
    EXPECT_EQ(r, (IntRect{55, 47, 50, 50}));
    const IntRect filled = regionScissorRect(ShapePoints::circle({80, 72}, 20), 1, kW, kH);
    EXPECT_GT(r.width, filled.width);  // the stroke half-width widens the box beyond the filled reach
}

// ── Coordinate space + clamping ─────────────────────────────────────────────────────────

TEST(RegionScissor, ComposeScale2Scales) {
    // Same circle as CircleBoundingBox, but the intermediates are viewport×2 (320×288): every coordinate
    // doubles. The viewport box [49,111]×[41,103] scales to [98,222]×[82,206].
    const IntRect r = regionScissorRect(ShapePoints::circle({80, 72}, 30), 2, kW * 2, kH * 2);
    EXPECT_EQ(r, (IntRect{98, 82, 124, 124}));
}

TEST(RegionScissor, ClampsToViewportEdges) {
    // Bottom-right: box runs off the far edges → clamped to composeW/composeH.
    const IntRect br = regionScissorRect(ShapePoints::circle({150, 140}, 30), 1, kW, kH);
    EXPECT_EQ(br, (IntRect{119, 109, 41, 35}));  // x1 clamped 181→160, y1 clamped 171→144
    // Top-left: box runs negative → clamped to the origin.
    const IntRect tl = regionScissorRect(ShapePoints::circle({10, 10}, 30), 1, kW, kH);
    EXPECT_EQ(tl, (IntRect{0, 0, 41, 41}));  // x0 clamped -21→0, y0 clamped -21→0
}

TEST(RegionScissor, FullyOffscreenCollapsesTo1x1) {
    // Entirely past the right edge → clamped to nothing in x → a 1×1 valid scissor at the clamped corner
    // (backends need not accept a zero-area scissor; the gate reads nothing there anyway).
    const IntRect r = regionScissorRect(ShapePoints::circle({300, 72}, 10), 1, kW, kH);
    EXPECT_EQ(r.width, 1);
    EXPECT_EQ(r.height, 1);
    EXPECT_LE(r.x, kW - 1);  // corner clamped to composeW-1
    EXPECT_GE(r.x, 0);
    EXPECT_LE(r.y, kH - 1);
}

// ── Bails → the full compose rect ("no scissor") ────────────────────────────────────────

TEST(RegionScissor, InvertBailsToFullRect) {
    const IntRect r = regionScissorRect(ShapePoints::circle({80, 72}, 30).inverted(), 1, kW, kH);
    EXPECT_EQ(r, (IntRect{0, 0, kW, kH}));  // the region is the OUTSIDE → spans the frame
}

TEST(RegionScissor, TransformBailsToFullRect) {
    ShapePoints warped = ShapePoints::circle({80, 72}, 30);
    warped.transform   = Transform::translation(5, 5);  // any non-identity transform → full rect
    EXPECT_EQ(regionScissorRect(warped, 1, kW, kH), (IntRect{0, 0, kW, kH}));
}

TEST(RegionScissor, CurveBailsToFullRect) {
    ShapePoints curved = ShapePoints::circle({80, 72}, 30);
    curved.curve.push_back(CurveSegment{});  // a curved boundary is not analytically bounded here
    EXPECT_EQ(regionScissorRect(curved, 1, kW, kH), (IntRect{0, 0, kW, kH}));
}

TEST(RegionScissor, EmptyPointsBailsToFullRect) {
    EXPECT_EQ(regionScissorRect(ShapePoints{}, 1, kW, kH), (IntRect{0, 0, kW, kH}));  // no region
}

}  // namespace
}  // namespace retropp
