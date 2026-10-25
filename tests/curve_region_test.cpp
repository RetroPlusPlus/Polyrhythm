#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <cmath>
#include <span>
#include <vector>

#include "retropp/curve.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// Curved effect-region boundaries (analytic linear + quadratic). Device-free coverage of the CPU side
// the region_select_curve.frag shader mirrors: sdCurveAnalytic / curveRegionContains / curveRegionParams
// / curveRegionIsAnalytic / ShapePoints::fromCurve (postprocess.h + draw_state.h), verified against
// Curve::signedDistance (the reference). The live curve gate is build-compiled + dev-verified on all
// three backends; these are the failable units.

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

[[nodiscard]] CurveSegment line(Vec2 a, Vec2 b) {
    return CurveSegment{a, b, Vec2{}, Vec2{}, CurveDegree::Linear};
}
[[nodiscard]] CurveSegment quad(Vec2 a, Vec2 ctrl, Vec2 b) {
    return CurveSegment{a, ctrl, b, Vec2{}, CurveDegree::Quadratic};
}

// A closed all-Linear square (20,20)–(120,120): the boundary whose analytic distance must reduce to the
// polygon answer (linear = exact point-to-segment).
[[nodiscard]] Curve linearSquare() {
    Curve c;
    c.closed   = true;
    c.segments = {line({20, 20}, {120, 20}), line({120, 20}, {120, 120}),
                  line({120, 120}, {20, 120}), line({20, 120}, {20, 20})};
    return c;
}

// A closed "D" — a quadratic arc bulging UP to apex (80,60) over a flat base chord at y=100. Interior is
// between the arc and the base. The arc's true position diverges from any coarse chord facet.
[[nodiscard]] Curve dShape() {
    Curve c;
    c.closed   = true;
    c.segments = {quad({40, 100}, {80, 20}, {120, 100}),  // arc (apex at the midpoint (80,60))
                  line({120, 100}, {40, 100})};            // base chord
    return c;
}

// ── sdCurveAnalytic vs Curve::signedDistance — the source-of-truth check ────────────────

TEST(SdCurveAnalytic, LinearSquareMatchesSignedDistanceSign) {
    const Curve sq = linearSquare();
    const std::span<const CurveSegment> segs(sq.segments);
    // Center inside (negative), a clear outside point (positive), an on-edge point (≈ 0).
    EXPECT_LT(sdCurveAnalytic({70, 70}, segs), 0.0f);
    EXPECT_GT(sdCurveAnalytic({10, 70}, segs), 0.0f);
    EXPECT_LT(std::abs(sdCurveAnalytic({20, 70}, segs)), 0.5f);
    // Sign agreement with the reference at every probe.
    EXPECT_EQ(sdCurveAnalytic({70, 70}, segs) < 0.0f, sq.signedDistance({70, 70}) < 0.0f);
    EXPECT_EQ(sdCurveAnalytic({10, 70}, segs) < 0.0f, sq.signedDistance({10, 70}) < 0.0f);
}

TEST(SdCurveAnalytic, LinearSquareMatchesSignedDistanceMagnitude) {
    const Curve sq = linearSquare();
    const std::span<const CurveSegment> segs(sq.segments);
    for (const Point p : {Point{70, 70}, Point{10, 70}, Point{30, 25}, Point{200, 70}}) {
        EXPECT_NEAR(std::abs(sdCurveAnalytic(p, segs)), std::abs(sq.signedDistance({p.x, p.y})), 0.5f)
            << "at (" << p.x << "," << p.y << ")";
    }
}

TEST(SdCurveAnalytic, QuadraticBoundarySignAgreesWithReference) {
    const Curve d = dShape();
    const std::span<const CurveSegment> segs(d.segments);
    // Inside the D (below the arc, above the base), clearly outside (above the apex), and near-boundary.
    for (const Point p : {Point{80, 85}, Point{60, 90}, Point{80, 30}, Point{80, 130}, Point{60, 75}}) {
        EXPECT_EQ(sdCurveAnalytic(p, segs) < 0.0f, d.signedDistance({p.x, p.y}) < 0.0f)
            << "sign disagreement at (" << p.x << "," << p.y << ")";
    }
}

TEST(SdCurveAnalytic, QuadraticBoundaryMagnitudeWithinTolerance) {
    const Curve d = dShape();
    const std::span<const CurveSegment> segs(d.segments);
    for (const Point p : {Point{80, 85}, Point{80, 30}, Point{45, 70}}) {
        EXPECT_NEAR(std::abs(sdCurveAnalytic(p, segs)), std::abs(d.signedDistance({p.x, p.y})), 1.0f)
            << "at (" << p.x << "," << p.y << ")";
    }
}

// A closed rounded outline of four quadratic Bezier segments about (cx,cy), axis radius r — the demo's
// blob, and a richer corpus than the two-segment D for the sign-agreement sweep.
[[nodiscard]] Curve roundedQuad(float cx, float cy, float r) {
    const Vec2 n{cx, cy - r}, e{cx + r, cy}, s{cx, cy + r}, w{cx - r, cy};
    Curve c;
    c.closed   = true;
    c.segments = {quad(n, {cx + r, cy - r}, e), quad(e, {cx + r, cy + r}, s),
                  quad(s, {cx - r, cy + r}, w), quad(w, {cx - r, cy - r}, n)};
    return c;
}

// The core guarantee: the analytic crossing-number sign must agree with the reference's dense-sample
// winding everywhere on a well-formed closed boundary. Sweep a grid across the viewport over the rounded
// quadratic blob and assert sign agreement at every probe (boundary-row points are skipped — the
// measure-zero exact-edge scanline case is not the contract).
TEST(SdCurveAnalytic, SignAgreesWithReferenceAcrossAGrid) {
    const Curve blob = roundedQuad(80.0f, 72.0f, 40.0f);
    const std::span<const CurveSegment> segs(blob.segments);
    int checked = 0;
    for (int gy = 8; gy < 144; gy += 7) {
        for (int gx = 8; gx < 160; gx += 7) {
            const Point p{static_cast<float>(gx) + 0.5f, static_cast<float>(gy) + 0.5f};
            const float ref = blob.signedDistance({p.x, p.y});
            if (std::abs(ref) < 1.5f) continue;  // skip the near-boundary band (sign is ambiguous there)
            EXPECT_EQ(sdCurveAnalytic(p, segs) < 0.0f, ref < 0.0f)
                << "sign disagreement at (" << p.x << "," << p.y << ")";
            ++checked;
        }
    }
    EXPECT_GT(checked, 200);  // the sweep actually exercised a broad corpus
}

// ── The headline property: exact between control points, where a coarse facet misses ────

TEST(SdCurveAnalytic, ExactWhereACoarsePolygonFacetMisclassifies) {
    // The true quadratic arc at x=60 sits at y≈70; a coarse 3-vertex facet of the same arc cuts the
    // corner at y=80. The point (60,75) is below the true arc (INSIDE the D) but above the facet
    // (OUTSIDE the triangle) — the no-facets win, asserted as a sign contrast.
    const Curve d = dShape();
    EXPECT_LT(sdCurveAnalytic({60, 75}, std::span<const CurveSegment>(d.segments)), 0.0f);

    // The same arc sampled to 3 points (t = 0, 0.5, 1) + closed → a triangle; the facet misses (60,75).
    const std::vector<Point> coarse = {{40, 100}, {80, 60}, {120, 100}};
    EXPECT_GT(sdPolygon({60, 75}, std::span<const Point>(coarse)), 0.0f);
}

// ── curveRegionContains — radius + transform compose like the polygon gate ──────────────

TEST(CurveRegionContains, NoCurveDefersToPolygonGate) {
    // A curve-free region routes through the polygon gate, byte-identical to the shipped path.
    const ShapePoints poly = ShapePoints::circle({80, 72}, 30);
    EXPECT_TRUE(curveRegionContains({80, 72}, poly));
    EXPECT_FALSE(curveRegionContains({80, 110}, poly));
}

TEST(CurveRegionContains, RadiusInflationFlipsAJustOutsidePoint) {
    ShapePoints region;
    region.curve = dShape().segments;
    const Point probe{80, 55};  // 5 px above the apex (80,60) — just outside
    EXPECT_FALSE(curveRegionContains(probe, region));
    region.radius = 10.0f;
    EXPECT_TRUE(curveRegionContains(probe, region));
}

TEST(CurveRegionContains, TransformTranslationMovesTheCurve) {
    // A small leaf centered at (40,72): two quadratics bulging above/below the (20,72)–(60,72) spine.
    ShapePoints region;
    region.curve = {quad({20, 72}, {40, 52}, {60, 72}), quad({60, 72}, {40, 92}, {20, 72})};
    EXPECT_TRUE(curveRegionContains({40, 72}, region));    // the leaf center
    EXPECT_FALSE(curveRegionContains({100, 72}, region));  // far to the right
    region.transform = Transform::translation(60, 0);       // shape center effectively at (100,72)
    EXPECT_TRUE(curveRegionContains({100, 72}, region));
}

TEST(CurveRegionContains, TransformScaleEnlargesTheCurve) {
    // A small leaf centered at (40,72) spanning y ∈ [62,82] (apexes ±10 off the spine).
    ShapePoints region;
    region.curve = {quad({30, 72}, {40, 52}, {50, 72}), quad({50, 72}, {40, 92}, {30, 72})};
    const Point probe{40, 95};  // 23 px below center — well outside the small leaf (bottom at y=82)
    EXPECT_FALSE(curveRegionContains(probe, region));
    region.transform = Transform::scale(3.0f, 3.0f, 40.0f, 72.0f);  // 3× about the center → reaches y=102
    EXPECT_TRUE(curveRegionContains(probe, region));
}

TEST(CurveRegionContains, InvertFlipsInsideAndOutside) {
    ShapePoints region = ShapePoints::fromCurve(roundedQuad(80.0f, 72.0f, 30.0f));
    EXPECT_TRUE(curveRegionContains({80, 72}, region));    // center — inside the curve
    EXPECT_FALSE(curveRegionContains({80, 130}, region));  // well outside
    region.invert = true;                                  // the region becomes the OUTSIDE of the curve
    EXPECT_FALSE(curveRegionContains({80, 72}, region));
    EXPECT_TRUE(curveRegionContains({80, 130}, region));
}

// ── strokeWidth — curved stroke / outline (closed loop + open path) ─────────────────────

TEST(CurveRegionStroke, ClosedCurveStrokeIsABandAlongTheBoundary) {
    ShapePoints region = ShapePoints::fromCurve(roundedQuad(80.0f, 72.0f, 30.0f));
    region.strokeWidth = 8.0f;                            // band |signedDistance| ≤ 4 along the rounded outline
    EXPECT_FALSE(curveRegionContains({80, 72}, region));  // the center (deep interior) → not in the stroke
    EXPECT_TRUE(curveRegionContains({80, 102}, region));  // the south on-curve point → on the band
    EXPECT_FALSE(curveRegionContains({80, 130}, region)); // well past the outer edge → excluded
}

TEST(CurveRegionStroke, OpenPathStrokesIntoAnOpenBand) {
    // An OPEN polyline (no closing chord) — stroke is sign-independent (|signedDistance|), so an effect
    // follows the path as a thin band with no filled interior on either side.
    ShapePoints path;
    path.curve       = {line({20, 72}, {80, 72}), line({80, 72}, {140, 72})};  // open horizontal path
    path.strokeWidth = 8.0f;                                                   // band of width 8 about the line
    EXPECT_TRUE(curveRegionContains({80, 72}, path));    // on the path → in the band
    EXPECT_TRUE(curveRegionContains({80, 75}, path));    // 3 px off → within ±4
    EXPECT_FALSE(curveRegionContains({80, 82}, path));   // 10 px off the path → out of the band
    EXPECT_FALSE(curveRegionContains({80, 120}, path));  // far from the path → not a filled region
}

// ── curveRegionIsAnalytic — the renderer's curve/sample routing decision ────────────────

TEST(CurveRegionIsAnalytic, LinearAndQuadraticAreAnalytic) {
    const std::vector<CurveSegment> segs = {line({0, 0}, {10, 0}), quad({10, 0}, {15, 10}, {20, 0})};
    EXPECT_TRUE(curveRegionIsAnalytic(std::span<const CurveSegment>(segs)));
    const std::vector<CurveSegment> empty;
    EXPECT_TRUE(curveRegionIsAnalytic(std::span<const CurveSegment>(empty)));  // vacuously
}

TEST(CurveRegionIsAnalytic, CubicFallsBack) {
    // Catmull-Rom authors into Cubic → the sampled-polygon fallback.
    const Vec2 pts[] = {{0, 0}, {30, 40}, {60, 0}, {30, -40}};
    const Curve cr   = Curve::throughPoints(pts, /*closed=*/true);
    EXPECT_FALSE(curveRegionIsAnalytic(std::span<const CurveSegment>(cr.segments)));
}

// ── ShapePoints::fromCurve + the byte-identical empty sentinel ──────────────────────────

TEST(FromCurve, BuildsACurveRegion) {
    const Curve c = Curve::quadratic({0, 0}, {50, -30}, {100, 0});
    const ShapePoints r = ShapePoints::fromCurve(c, 5.0f);
    EXPECT_EQ(r.curve, c.segments);  // the boundary segments equal the source curve's
    EXPECT_TRUE(r.points.empty());   // the curve path, not the polygon path
    EXPECT_FLOAT_EQ(r.radius, 5.0f);
    EXPECT_TRUE(r.hasRegion());
}

TEST(FromCurve, DefaultShapePointsHasNoCurve) {
    const ShapePoints def;  // the byte-identical sentinel must survive the new member
    EXPECT_TRUE(def.curve.empty());
    EXPECT_TRUE(def.points.empty());
    EXPECT_FALSE(def.hasRegion());
}

TEST(ScreenSpaceEffect, TransparencyTakesACurvedBoundaryFromItsRegion) {
    // A Transparency carries no geometry of its own; a curved see-through boundary comes from the Region
    // that owns it. The stencil() sugar builds that Region from a fromCurve shape.
    const Curve c = Curve::quadratic({40, 40}, {80, 10}, {120, 40});
    const std::vector<Region> regions = stencil(ShapePoints::fromCurve(c));
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].shape.curve, c.segments);  // the see-through boundary follows the curve
    EXPECT_EQ(regions[0].effects[0].kind, ScreenSpaceEffectKind::Transparency);
}

// ── curveRegionParams — the cbuffer mirror ──────────────────────────────────────────────

TEST(CurveRegionParamsResolve, SegmentCountRadiusAndInverseViewport) {
    ShapePoints region;
    region.curve  = dShape().segments;  // 1 quadratic + 1 linear
    region.radius = 12.0f;
    const CurveRegionParams p = curveRegionParams(region, kViewport);
    EXPECT_EQ(p.segmentCount, 2u);
    EXPECT_FLOAT_EQ(p.radius, 12.0f);
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
    EXPECT_FLOAT_EQ(p.invRow0[0], 1.0f);  // identity transform → identity inverse
    EXPECT_FLOAT_EQ(p.invRow1[1], 1.0f);
    EXPECT_FLOAT_EQ(p.invRow2[2], 1.0f);
}

TEST(CurveRegionParamsResolve, TransformInverseIsResolved) {
    ShapePoints region;
    region.curve     = {quad({0, 0}, {10, 10}, {20, 0}), line({20, 0}, {0, 0})};
    region.transform = Transform::translation(60, 0);  // inverse is translate(-60, 0)
    const CurveRegionParams p = curveRegionParams(region, kViewport);
    EXPECT_FLOAT_EQ(p.invRow0[2], -60.0f);
}

TEST(CurveRegionParamsResolve, CarriesStrokeWidth) {
    ShapePoints region;
    region.curve       = dShape().segments;
    region.strokeWidth = 6.0f;
    EXPECT_FLOAT_EQ(curveRegionParams(region, kViewport).strokeWidth, 6.0f);  // the shader's uInvRow1.w
}

}  // namespace
}  // namespace retropp
