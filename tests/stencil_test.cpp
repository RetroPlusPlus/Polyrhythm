#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <span>

#include "retropp/curve.h"
#include "retropp/geometry.h"

// Stencil (region see-through) — device-free coverage of the CPU side the GPU region_stencil.frag /
// region_stencil_curve.frag mirror: the feathered survival math (stencilCoverage / stencilSurvival),
// its composition with the shipped region SDF (sdPolygon / sdCurveAnalytic), the no-region degenerate,
// and the cbuffer param resolvers (stencilParams / curveStencilParams). The live see-through is build-compiled
// + dev-verified across all three backends (the documented CI-headless boundary); these are the failable
// units.

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// ── stencilCoverage — the feathered boundary ramp (constexpr, static_assert) ────────────

// Hard edge (feather 0): 1 strictly inside, 0 strictly outside, 1 on the boundary (signedDist <= 0).
static_assert(stencilCoverage(-1.0f, 0.0f) == 1.0f);
static_assert(stencilCoverage(1.0f, 0.0f) == 0.0f);
static_assert(stencilCoverage(0.0f, 0.0f) == 1.0f);

// Feathered (feather 4): 0.5 at the boundary, 1 at -feather/2, 0 at +feather/2, clamped past the ends.
static_assert(stencilCoverage(0.0f, 4.0f) == 0.5f);
static_assert(stencilCoverage(-2.0f, 4.0f) == 1.0f);
static_assert(stencilCoverage(2.0f, 4.0f) == 0.0f);
static_assert(stencilCoverage(-10.0f, 4.0f) == 1.0f);  // far inside → clamped to 1
static_assert(stencilCoverage(10.0f, 4.0f) == 0.0f);   // far outside → clamped to 0

// ── stencilSurvival — mode-selected survival (constexpr, static_assert) ─────────────────

static_assert(stencilSurvival(StencilMode::TransparentInside, 1.0f) == 0.0f);   // coverage 1 inside → transparent
static_assert(stencilSurvival(StencilMode::TransparentInside, 0.0f) == 1.0f);   // coverage 0 outside → kept
static_assert(stencilSurvival(StencilMode::TransparentInside, 0.5f) == 0.5f);   // boundary
static_assert(stencilSurvival(StencilMode::TransparentOutside, 1.0f) == 1.0f);  // coverage 1 inside → kept
static_assert(stencilSurvival(StencilMode::TransparentOutside, 0.0f) == 0.0f);  // coverage 0 outside → transparent
static_assert(stencilSurvival(StencilMode::TransparentOutside, 0.5f) == 0.5f);

TEST(StencilCoverage, HardEdgeIsAStep) {
    EXPECT_FLOAT_EQ(stencilCoverage(-5.0f, 0.0f), 1.0f);  // inside
    EXPECT_FLOAT_EQ(stencilCoverage(5.0f, 0.0f), 0.0f);   // outside
    EXPECT_FLOAT_EQ(stencilCoverage(0.0f, 0.0f), 1.0f);   // on the boundary
}

TEST(StencilCoverage, FeatheredRampIsCenteredOnTheBoundary) {
    EXPECT_FLOAT_EQ(stencilCoverage(0.0f, 8.0f), 0.5f);   // boundary → half
    EXPECT_FLOAT_EQ(stencilCoverage(-4.0f, 8.0f), 1.0f);  // -feather/2 → full inside
    EXPECT_FLOAT_EQ(stencilCoverage(4.0f, 8.0f), 0.0f);   // +feather/2 → full outside
    EXPECT_FLOAT_EQ(stencilCoverage(-1.0f, 8.0f), 0.625f);  // partway in
    EXPECT_FLOAT_EQ(stencilCoverage(1.0f, 8.0f), 0.375f);   // partway out
}

TEST(StencilSurvival, ModeSelectsWhichSideGoesTransparent) {
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentInside, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentInside, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentOutside, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentOutside, 0.0f), 0.0f);
}

// ── Composition with the shipped polygon SDF ────────────────────────────────────────────

// Survival at a fragment for a polygon region (identity transform → fragment is shape-local), composing
// the real sdPolygon with the stencil math exactly as the shader does.
[[nodiscard]] float polySurvival(Point frag, const ShapePoints& region, StencilMode mode, float feather) {
    const float sd = sdPolygon(frag, std::span<const Point>(region.points)) - region.radius;
    return stencilSurvival(mode, stencilCoverage(sd, feather));
}

TEST(StencilPolygon, TransparentInsidePunchesAHole) {
    const ShapePoints circle = ShapePoints::circle({80, 72}, 30);
    EXPECT_FLOAT_EQ(polySurvival({80, 72}, circle, StencilMode::TransparentInside, 0.0f), 0.0f);   // center transparent
    EXPECT_FLOAT_EQ(polySurvival({80, 120}, circle, StencilMode::TransparentInside, 0.0f), 1.0f);  // 48 px out, kept
}

TEST(StencilPolygon, TransparentOutsideKeepsAPorthole) {
    const ShapePoints circle = ShapePoints::circle({80, 72}, 30);
    EXPECT_FLOAT_EQ(polySurvival({80, 72}, circle, StencilMode::TransparentOutside, 0.0f), 1.0f);   // center kept
    EXPECT_FLOAT_EQ(polySurvival({80, 120}, circle, StencilMode::TransparentOutside, 0.0f), 0.0f);  // outside transparent
}

TEST(StencilPolygon, FeatheredEdgeHasSignalNearTheBoundary) {
    // A point 2 px inside a circle's edge, with a wide feather, survives partially (strictly 0..1) under
    // TransparentInside — the soft edge isn't a hard 0 or 1.
    const ShapePoints circle = ShapePoints::circle({80, 72}, 30);
    const float s = polySurvival({80, 100}, circle, StencilMode::TransparentInside, 12.0f);  // dist 28 → sd -2
    EXPECT_GT(s, 0.0f);
    EXPECT_LT(s, 1.0f);
}

// ── No-region degenerate ────────────────────────────────────────────────────────────────

// An empty region means "whole viewport inside" → coverage 1 (the shader's count==0 branch). TransparentInside
// then makes the whole layer transparent; TransparentOutside is a no-op.
TEST(StencilNoRegion, TransparentInsideMakesEverythingTransparent) {
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentInside, 1.0f), 0.0f);
}

TEST(StencilNoRegion, TransparentOutsideIsANoOp) {
    EXPECT_FLOAT_EQ(stencilSurvival(StencilMode::TransparentOutside, 1.0f), 1.0f);
}

// ── Composition with the analytic curve SDF ─────────────────────────────────────────────

// A closed rounded outline of four quadratic Bezier segments about (cx,cy) with axis radius r — the
// analytic-curve case (its distance has a closed form).
[[nodiscard]] Curve roundedQuad(float cx, float cy, float r) {
    const Vec2 n{cx, cy - r}, e{cx + r, cy}, s{cx, cy + r}, w{cx - r, cy};
    const Vec2 ne{cx + r, cy - r}, se{cx + r, cy + r}, sw{cx - r, cy + r}, nw{cx - r, cy - r};
    Curve c;
    c.closed   = true;
    c.segments = {CurveSegment{n, ne, e, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{e, se, s, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{s, sw, w, Vec2{}, CurveDegree::Quadratic},
                  CurveSegment{w, nw, n, Vec2{}, CurveDegree::Quadratic}};
    return c;
}

[[nodiscard]] float curveSurvival(Point frag, const ShapePoints& region, StencilMode mode, float feather) {
    const float sd =
        sdCurveAnalytic(frag, std::span<const CurveSegment>(region.curve)) - region.radius;
    return stencilSurvival(mode, stencilCoverage(sd, feather));
}

TEST(StencilCurve, TransparentInsideMakesInsideTransparent) {
    const ShapePoints region = ShapePoints::fromCurve(roundedQuad(80, 72, 30));
    EXPECT_FLOAT_EQ(curveSurvival({80, 72}, region, StencilMode::TransparentInside, 0.0f), 0.0f);  // inside transparent
    EXPECT_FLOAT_EQ(curveSurvival({80, 130}, region, StencilMode::TransparentInside, 0.0f), 1.0f); // outside kept
}

TEST(StencilCurve, TransparentOutsideKeepsInsideTheCurve) {
    const ShapePoints region = ShapePoints::fromCurve(roundedQuad(80, 72, 30));
    EXPECT_FLOAT_EQ(curveSurvival({80, 72}, region, StencilMode::TransparentOutside, 0.0f), 1.0f);  // inside kept
    EXPECT_FLOAT_EQ(curveSurvival({80, 130}, region, StencilMode::TransparentOutside, 0.0f), 0.0f); // outside transparent
}

TEST(StencilCurve, AnalyticBoundaryIsAnalytic) {
    const ShapePoints region = ShapePoints::fromCurve(roundedQuad(80, 72, 30));
    EXPECT_TRUE(curveRegionIsAnalytic(std::span<const CurveSegment>(region.curve)));
}

TEST(StencilCurve, CubicBoundaryRoutesToTheSampledFallback) {
    // A Catmull-Rom curve (throughPoints) produces cubic segments; the analytic stencil shader can't
    // evaluate them, so the renderer samples the boundary to a polygon (region_stencil.frag). The decision
    // helper drives that.
    const std::array<Vec2, 4> pts{{{40, 40}, {120, 40}, {120, 100}, {40, 100}}};
    const Curve cr = Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);
    EXPECT_FALSE(curveRegionIsAnalytic(std::span<const CurveSegment>(cr.segments)));
}

// ── Cbuffer param resolvers ──────────────────────────────────────────────────────────────

TEST(StencilParamsResolve, CarriesModeFeatherAndRegion) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    const StencilParams p = stencilParams(c, StencilMode::TransparentOutside, 6.0f, kViewport);
    EXPECT_EQ(p.mode, 1u);  // TransparentOutside
    EXPECT_FLOAT_EQ(p.feather, 6.0f);
    EXPECT_EQ(p.region.count, 1u);
    EXPECT_FLOAT_EQ(p.region.radius, 30.0f);
    EXPECT_FLOAT_EQ(p.region.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.region.invViewportH, 1.0f / 144.0f);
}

TEST(StencilParamsResolve, TransparentInsideIsModeZero) {
    const StencilParams p =
        stencilParams(ShapePoints::circle({0, 0}, 1), StencilMode::TransparentInside, 0.0f, kViewport);
    EXPECT_EQ(p.mode, 0u);
    EXPECT_FLOAT_EQ(p.feather, 0.0f);
}

TEST(CurveStencilParamsResolve, CarriesModeFeatherAndSegments) {
    const ShapePoints region = ShapePoints::fromCurve(roundedQuad(80, 72, 30));
    const CurveStencilParams p = curveStencilParams(region, StencilMode::TransparentOutside, 4.0f, kViewport);
    EXPECT_EQ(p.mode, 1u);
    EXPECT_FLOAT_EQ(p.feather, 4.0f);
    EXPECT_EQ(p.region.segmentCount, 4u);  // four quadratic segments
    EXPECT_FLOAT_EQ(p.region.radius, 0.0f);
    EXPECT_FLOAT_EQ(p.region.invViewportW, 1.0f / 160.0f);
}

}  // namespace
}  // namespace retropp
