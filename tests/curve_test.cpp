// Device-free tests for the Curve primitive. Pure CPU: evaluation (de Casteljau endpoints + a
// hand-computed cubic midpoint + unit tangents), the three authoring conversions (Catmull-Rom passes
// through its points; Hermite endpoint tangents; line == a Tween<Vec2> lerp), arc length +
// constant-speed atDistance, and the signedDistance field (analytic Linear/Quadratic vs a dense
// brute-force reference, cubic within subdivision tolerance, winding sign for closed loops).

#include "retropp/curve.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>

#include "retropp/tween.h"  // retropp::lerp(Vec2,…) — the consistency cross-check for Curve::line

#include <gtest/gtest.h>

using namespace retropp;

namespace {

constexpr float kPi = 3.14159265358979323846f;

void expectVecNear(Vec2 got, Vec2 want, float tol) {
    EXPECT_NEAR(got.x, want.x, tol);
    EXPECT_NEAR(got.y, want.y, tol);
}

float dist(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Brute-force dense-sample minimum distance — the independent reference the analytic/subdivision
// signedDistance is checked against.
float bruteUnsigned(const Curve& c, Vec2 p, int n = 4000) {
    float best = std::numeric_limits<float>::infinity();
    for (int i = 0; i <= n; ++i) {
        best = std::min(best, dist(p, c.at(static_cast<float>(i) / static_cast<float>(n))));
    }
    return best;
}

}  // namespace

// ── Evaluation ─────────────────────────────────────────────────────────────────────────────────

TEST(Curve, LineEndpoints) {
    const Curve c = Curve::line(Vec2{0, 0}, Vec2{10, 4});
    expectVecNear(c.at(0.0f), Vec2{0, 0}, 1e-5f);
    expectVecNear(c.at(1.0f), Vec2{10, 4}, 1e-5f);
    expectVecNear(c.at(0.5f), Vec2{5, 2}, 1e-5f);
}

TEST(Curve, QuadraticEndpoints) {
    const Curve c = Curve::quadratic(Vec2{0, 0}, Vec2{5, 10}, Vec2{10, 0});
    expectVecNear(c.at(0.0f), Vec2{0, 0}, 1e-5f);
    expectVecNear(c.at(1.0f), Vec2{10, 0}, 1e-5f);
    // B(0.5) = 0.25 p0 + 0.5 ctrl + 0.25 p1 = (5, 5)
    expectVecNear(c.at(0.5f), Vec2{5, 5}, 1e-5f);
}

TEST(Curve, CubicEndpointsAndHandComputedMidpoint) {
    const Curve c = Curve::cubic(Vec2{0, 0}, Vec2{0, 10}, Vec2{10, 10}, Vec2{10, 0});
    expectVecNear(c.at(0.0f), Vec2{0, 0}, 1e-5f);
    expectVecNear(c.at(1.0f), Vec2{10, 0}, 1e-5f);
    // B(0.5) = (1/8)(p0 + 3c0 + 3c1 + p3) = (1/8)(40, 60) = (5, 7.5)
    expectVecNear(c.at(0.5f), Vec2{5.0f, 7.5f}, 1e-5f);
}

TEST(Curve, TangentIsUnitAndCorrectHeading) {
    // Line heading +x → tangent (1,0) everywhere.
    const Curve line = Curve::line(Vec2{0, 0}, Vec2{10, 0});
    expectVecNear(line.tangent(0.0f), Vec2{1, 0}, 1e-5f);
    expectVecNear(line.tangent(0.5f), Vec2{1, 0}, 1e-5f);

    // Cubic leaving straight up (c0 above p0) and arriving straight down (c1 above p3).
    const Curve c = Curve::cubic(Vec2{0, 0}, Vec2{0, 10}, Vec2{10, 10}, Vec2{10, 0});
    const Vec2 t0 = c.tangent(0.0f);
    const Vec2 t1 = c.tangent(1.0f);
    EXPECT_NEAR(std::sqrt(t0.x * t0.x + t0.y * t0.y), 1.0f, 1e-5f);  // unit
    EXPECT_NEAR(std::sqrt(t1.x * t1.x + t1.y * t1.y), 1.0f, 1e-5f);
    expectVecNear(t0, Vec2{0, 1}, 1e-5f);
    expectVecNear(t1, Vec2{0, -1}, 1e-5f);
}

// ── Authoring conversions ────────────────────────────────────────────────────────────────────────

TEST(Curve, LineEqualsTweenLerp) {
    const Vec2 a{3, 7}, b{40, -12};
    const Curve c = Curve::line(a, b);
    for (float t : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f}) {
        expectVecNear(c.at(t), lerp(a, b, t), 1e-5f);  // Curve::line IS a Tween<Vec2> lerp
    }
}

TEST(Curve, ThroughPointsPassesThroughEveryPointOpen) {
    const std::array<Vec2, 4> pts{{{0, 0}, {30, 50}, {70, 20}, {100, 80}}};
    const Curve c = Curve::throughPoints(std::span<const Vec2>(pts), false);
    ASSERT_EQ(c.count(), 3u);  // 4 points → 3 spans
    // Each segment join lands ON the corresponding waypoint.
    expectVecNear(c.at(0.0f), pts[0], 1e-4f);
    expectVecNear(c.at(1.0f / 3.0f), pts[1], 1e-4f);
    expectVecNear(c.at(2.0f / 3.0f), pts[2], 1e-4f);
    expectVecNear(c.at(1.0f), pts[3], 1e-4f);
}

TEST(Curve, ThroughPointsPassesThroughEveryPointClosed) {
    const std::array<Vec2, 4> pts{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}};
    const Curve c = Curve::throughPoints(std::span<const Vec2>(pts), true);
    ASSERT_EQ(c.count(), 4u);  // closed → one span per point (wraps last→first)
    EXPECT_TRUE(c.closed);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(pts.size());
        expectVecNear(c.at(t), pts[i], 1e-4f);
    }
}

TEST(Curve, HermiteEndpointTangentsMatchSupplied) {
    const Vec2 p0{0, 0}, p1{100, 0};
    const Vec2 t0{0, 60}, t1{0, -60};  // leave upward, arrive downward
    const Curve c = Curve::hermite(p0, t0, p1, t1);
    expectVecNear(c.at(0.0f), p0, 1e-5f);
    expectVecNear(c.at(1.0f), p1, 1e-5f);
    // Unit tangent at the endpoints equals the normalized supplied tangents.
    expectVecNear(c.tangent(0.0f), Vec2{0, 1}, 1e-5f);
    expectVecNear(c.tangent(1.0f), Vec2{0, -1}, 1e-5f);
}

TEST(Curve, ChainableAppenders) {
    Curve c;
    c.lineTo(Vec2{10, 0}).lineTo(Vec2{10, 10});
    ASSERT_EQ(c.count(), 2u);
    expectVecNear(c.at(0.0f), Vec2{0, 0}, 1e-5f);   // starts at origin
    expectVecNear(c.at(1.0f), Vec2{10, 10}, 1e-5f);  // ends at the last `to`
}

// ── Arc length / constant-speed ──────────────────────────────────────────────────────────────────

TEST(Curve, LineLengthIsChordLength) {
    const Curve c = Curve::line(Vec2{0, 0}, Vec2{30, 40});  // 3-4-5 → 50
    EXPECT_NEAR(c.length(), 50.0f, 1e-3f);
}

TEST(Curve, QuarterCircleLength) {
    // Cubic Bézier approximation of a quarter circle, radius 100 → arc length ≈ π·100/2.
    constexpr float k = 0.5522847498f * 100.0f;
    const Curve c = Curve::cubic(Vec2{100, 0}, Vec2{100, k}, Vec2{k, 100}, Vec2{0, 100});
    EXPECT_NEAR(c.length(), kPi * 100.0f / 2.0f, 1.0f);
}

TEST(Curve, AtDistanceEndpointsAndConstantSpeed) {
    // Two segments of very different length so the arc-length query differs from the parameter query.
    Curve c;
    c.lineTo(Vec2{10, 0}).lineTo(Vec2{10, 90});  // first span 10 long, second 90 → total 100
    EXPECT_NEAR(c.length(), 100.0f, 1e-3f);
    expectVecNear(c.atDistance(0.0f), Vec2{0, 0}, 1e-4f);
    expectVecNear(c.atDistance(100.0f), Vec2{10, 90}, 1e-4f);
    // Arc-length midpoint: 50 along → 40 into the second segment → (10, 40).
    expectVecNear(c.atDistance(50.0f), Vec2{10, 40}, 1e-3f);
    // The PARAMETER midpoint is the segment boundary (10, 0) — distinct from the arc-length midpoint.
    expectVecNear(c.at(0.5f), Vec2{10, 0}, 1e-4f);
}

TEST(Curve, TangentAtDistanceIsUnitAndMatchesEndpoints) {
    const Curve c = Curve::cubic(Vec2{0, 0}, Vec2{0, 10}, Vec2{10, 10}, Vec2{10, 0});
    const Vec2 t0 = c.tangentAtDistance(0.0f);
    const Vec2 t1 = c.tangentAtDistance(c.length());
    EXPECT_NEAR(std::sqrt(t0.x * t0.x + t0.y * t0.y), 1.0f, 1e-4f);  // unit
    expectVecNear(t0, Vec2{0, 1}, 1e-3f);   // leaves straight up
    expectVecNear(t1, Vec2{0, -1}, 1e-3f);  // arrives straight down
}

TEST(Curve, TangentAtDistanceResolvesArcLengthNotParameter) {
    // Short horizontal segment then a long vertical one: arc-length 30 is well up the VERTICAL run.
    Curve c;
    c.lineTo(Vec2{10, 0}).lineTo(Vec2{10, 90});
    const float len = c.length();  // 100
    expectVecNear(c.tangentAtDistance(30.0f), Vec2{0, 1}, 1e-4f);  // facing up the vertical run
    // The PARAMETRIC tangent at the same fraction is still on the horizontal segment — the wrong heading
    // tangent(s / length()) would give, and the reason tangentAtDistance exists.
    expectVecNear(c.tangent(30.0f / len), Vec2{1, 0}, 1e-4f);
}

TEST(Curve, ArcTableMatchesOnCallQueries) {
    // The baked ArcLengthTable returns identical results to the Curve's own on-call queries — it only
    // skips the per-call resample.
    const std::array<Vec2, 4> pts{{{0, 0}, {30, 50}, {70, 20}, {100, 80}}};
    const Curve          c = Curve::throughPoints(std::span<const Vec2>(pts));
    const ArcLengthTable t = c.arcTable();
    EXPECT_NEAR(t.length(), c.length(), 1e-3f);
    for (float f : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
        const float s = f * c.length();
        expectVecNear(t.atDistance(s), c.atDistance(s), 1e-4f);
        expectVecNear(t.tangentAtDistance(s), c.tangentAtDistance(s), 1e-4f);
    }
}

// ── Signed distance ──────────────────────────────────────────────────────────────────────────────

TEST(Curve, ClosedTriangleSignAndMagnitude) {
    const Curve tri{{{Vec2{0, 0}, Vec2{100, 0}, Vec2{}, Vec2{}, CurveDegree::Linear},
                     {Vec2{100, 0}, Vec2{50, 100}, Vec2{}, Vec2{}, CurveDegree::Linear},
                     {Vec2{50, 100}, Vec2{0, 0}, Vec2{}, Vec2{}, CurveDegree::Linear}},
                    true};
    EXPECT_LT(tri.signedDistance(Vec2{50, 33}), 0.0f);   // centroid inside → negative
    EXPECT_GT(tri.signedDistance(Vec2{300, 300}), 0.0f);  // far outside → positive
    EXPECT_NEAR(tri.signedDistance(Vec2{50, 0}), 0.0f, 1e-3f);  // on the base edge → ≈ 0
    EXPECT_TRUE(tri.contains(Vec2{50, 33}));
    EXPECT_FALSE(tri.contains(Vec2{300, 300}));
}

TEST(Curve, OpenLinearDistanceMatchesExact) {
    const Curve c = Curve::line(Vec2{0, 0}, Vec2{100, 0});
    // Point above the segment midpoint → exact perpendicular distance 25.
    EXPECT_NEAR(c.signedDistance(Vec2{50, 25}), 25.0f, 1e-4f);
    // Past the end → distance to the endpoint.
    EXPECT_NEAR(c.signedDistance(Vec2{130, 0}), 30.0f, 1e-4f);
}

TEST(Curve, OpenQuadraticDistanceMatchesAnalyticReference) {
    const Curve c = Curve::quadratic(Vec2{0, 0}, Vec2{50, 80}, Vec2{100, 0});
    for (Vec2 p : {Vec2{50, 60}, Vec2{0, 50}, Vec2{120, 10}, Vec2{50, -20}}) {
        EXPECT_NEAR(c.signedDistance(p), bruteUnsigned(c, p), 0.1f);  // analytic vs dense reference
    }
}

TEST(Curve, OpenCubicDistanceWithinSubdivisionTolerance) {
    const Curve c = Curve::cubic(Vec2{0, 0}, Vec2{0, 100}, Vec2{100, 100}, Vec2{100, 0});
    for (Vec2 p : {Vec2{50, 50}, Vec2{50, 90}, Vec2{-10, 10}, Vec2{110, 5}}) {
        EXPECT_NEAR(c.signedDistance(p), bruteUnsigned(c, p), 0.2f);
    }
}

TEST(Curve, ClosedCatmullRomLoopContainment) {
    const std::array<Vec2, 4> pts{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}};  // a square's corners
    const Curve loop = Curve::throughPoints(std::span<const Vec2>(pts), true);
    EXPECT_TRUE(loop.contains(Vec2{50, 50}));    // center inside the smooth loop
    EXPECT_FALSE(loop.contains(Vec2{250, 50}));  // well outside
    EXPECT_LT(loop.signedDistance(Vec2{50, 50}), 0.0f);
    EXPECT_GT(loop.signedDistance(Vec2{250, 50}), 0.0f);
}

// ── Degenerate / guards ──────────────────────────────────────────────────────────────────────────

TEST(Curve, EmptyCurve) {
    const Curve c;
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.count(), 0u);
    expectVecNear(c.at(0.5f), Vec2{0, 0}, 0.0f);
    EXPECT_EQ(c.length(), 0.0f);
    EXPECT_TRUE(std::isinf(c.signedDistance(Vec2{1, 1})));
    EXPECT_FALSE(c.contains(Vec2{1, 1}));
}

TEST(Curve, SingleSegment) {
    const Curve c = Curve::cubic(Vec2{0, 0}, Vec2{10, 30}, Vec2{40, 30}, Vec2{50, 0});
    EXPECT_EQ(c.count(), 1u);
    EXPECT_FALSE(c.empty());
    EXPECT_GT(c.length(), 50.0f);  // the bowed cubic is longer than the 50-px chord
}

TEST(Curve, ZeroLengthTangentGuard) {
    const Curve c = Curve::line(Vec2{5, 5}, Vec2{5, 5});  // degenerate point segment
    expectVecNear(c.tangent(0.5f), Vec2{0, 0}, 0.0f);     // zero-length derivative → zero vector
}

TEST(Curve, OpenCurveContainsAlwaysFalse) {
    const Curve c = Curve::line(Vec2{0, 0}, Vec2{100, 0});
    EXPECT_FALSE(c.contains(Vec2{50, 0}));  // containment only defined for closed loops
}
