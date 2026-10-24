#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <cmath>
#include <span>
#include <vector>

#include "retropp/curve.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// Curved effect/stencil-region boundaries by a baked signed-distance mask (cubic / Catmull-Rom / arbitrary).
// Device-free coverage of the CPU side the region_select_curve_mask.frag / region_stencil_curve_mask.frag
// shaders mirror: bakeCurveMaskField / sampleCurveMaskField / curveMaskRegionContains / regionCurvePath
// (postprocess.h + draw_state.h), verified against Curve::signedDistance (the reference). The live mask gate
// is build-compiled + dev-verified on all three backends; these are the failable units.

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// A closed Catmull-Rom (cubic) loop through `n` points on a circle — the canonical cubic boundary the
// analytic path cannot solve, and the mask path does. The same points connected straight are the inscribed
// polygon a coarse facet would use.
[[nodiscard]] std::vector<Vec2> circlePoints(Vec2 c, float r, int n) {
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(n));
    constexpr float kTwoPi = 6.283185307179586f;
    for (int i = 0; i < n; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        pts.push_back(Vec2{c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return pts;
}

[[nodiscard]] Curve blob() {
    const std::vector<Vec2> pts = circlePoints({80, 72}, 45.0f, 6);
    return Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);
}

// The shape-local point at the center of texel (x, y) — the inverse of the field's local→grid map.
[[nodiscard]] Vec2 texelCenter(const CurveMaskField& f, int x, int y) {
    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(f.width);
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(f.height);
    return Vec2{f.bakeMin.x + u * f.bakeExtent.x, f.bakeMin.y + v * f.bakeExtent.y};
}

// ── Bake fidelity — the field reproduces Curve::signedDistance ──────────────────────────────

TEST(BakeCurveMaskField, TexelsMatchSignedDistanceAtCenters) {
    const Curve          c = blob();
    Curve                closed = c;
    closed.closed          = true;
    const CurveMaskField f = bakeCurveMaskField(c, /*padding=*/8.0f, /*maxResolution=*/256);
    ASSERT_GT(f.width, 0);
    ASSERT_GT(f.height, 0);
    // Sample a sparse grid of texels; each stored distance equals signedDistance at that texel center.
    for (int y = 0; y < f.height; y += 17) {
        for (int x = 0; x < f.width; x += 17) {
            const Vec2  p    = texelCenter(f, x, y);
            const float want = closed.signedDistance(p);
            const float got  = f.distances[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) +
                                           static_cast<std::size_t>(x)];
            EXPECT_NEAR(got, want, 0.05f);
        }
    }
}

TEST(BakeCurveMaskField, BilinearReconstructionMatchesSignedDistance) {
    const Curve          c = blob();
    Curve                closed = c;
    closed.closed          = true;
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    // Off-texel-center probes across the interior — bilinear reconstruction tracks the true field.
    for (float py = 40.0f; py <= 104.0f; py += 9.0f) {
        for (float px = 48.0f; px <= 112.0f; px += 9.0f) {
            const Point p{px, py};
            EXPECT_NEAR(sampleCurveMaskField(f, p), closed.signedDistance({px, py}), 1.5f);
        }
    }
}

TEST(BakeCurveMaskField, ContainmentAgreesWithCurveAcrossAGrid) {
    const Curve          c      = blob();
    const CurveMaskField f      = bakeCurveMaskField(c, 8.0f, 256);
    Curve                closed = c;
    closed.closed               = true;
    int checked = 0;
    for (float py = 30.0f; py <= 114.0f; py += 4.0f) {
        for (float px = 30.0f; px <= 130.0f; px += 4.0f) {
            const float ref = closed.signedDistance({px, py});
            if (std::abs(ref) < 2.0f) continue;  // skip boundary-straddling probes (a sub-texel flip is noise)
            EXPECT_EQ(sampleCurveMaskField(f, {px, py}) < 0.0f, ref < 0.0f) << "at (" << px << "," << py << ")";
            ++checked;
        }
    }
    EXPECT_GT(checked, 100);  // the sweep actually exercised the field
}

// ── The headline property: exact where a coarse facet misclassifies ─────────────────────────

TEST(BakeCurveMaskField, ExactWhereACoarsePolygonFacetMisclassifies) {
    const std::vector<Vec2> pts = circlePoints({80, 72}, 45.0f, 6);
    const Curve             c   = Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);
    Curve                   closed = c;
    closed.closed                = true;
    const CurveMaskField    f   = bakeCurveMaskField(c, 8.0f, 256);

    // The inscribed hexagon (the points connected straight) is the coarse facet. Along the ray toward an
    // arc midpoint (between vertices 0 and 1), find a radius inside the true curve but OUTSIDE the hexagon —
    // the facet gap — and assert the mask classifies it by the true curve, where the polygon is wrong.
    std::vector<Point> hex;
    for (const Vec2& v : pts) hex.push_back(Point{v.x, v.y});
    const float midAngle = 6.283185307179586f * 0.5f / 6.0f;  // halfway between vertex 0 (0°) and vertex 1
    const Vec2  center{80, 72};
    int gapsFound = 0;
    for (float r = 36.0f; r <= 45.0f; r += 0.5f) {
        const Point probe{center.x + r * std::cos(midAngle), center.y + r * std::sin(midAngle)};
        const bool  insideCurve   = closed.signedDistance({probe.x, probe.y}) < 0.0f;
        const bool  insidePolygon = sdPolygon(probe, std::span<const Point>(hex)) < 0.0f;
        if (insideCurve && !insidePolygon) {  // the facet gap: inside the curve, outside the coarse hexagon
            EXPECT_LT(sampleCurveMaskField(f, probe), 0.0f) << "mask must classify the facet gap as inside";
            ++gapsFound;
        }
    }
    EXPECT_GT(gapsFound, 0) << "the coarse hexagon must miss some of the true curved boundary";
}

// ── radius / stroke / transform / invert compose on the sampled distance ────────────────────

TEST(CurveMaskRegionContains, RadiusInflationFlipsAJustOutsidePoint) {
    const Curve          c = blob();
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    Curve                closed = c;
    closed.closed          = true;
    // A probe a few px outside the boundary near the rightmost extent (~x=125, y=72; the curve reaches ~x=125).
    const Point probe{128, 72};
    ASSERT_GT(closed.signedDistance({probe.x, probe.y}), 0.0f);  // genuinely outside
    ShapePoints region = ShapePoints::fromCurve(c);
    region.curveMask   = static_cast<CurveMaskId>(1);  // a non-zero handle so regionCurvePath → Mask
    EXPECT_FALSE(curveMaskRegionContains(probe, region, f));  // radius 0 → outside
    region.radius = 12.0f;
    EXPECT_TRUE(curveMaskRegionContains(probe, region, f));   // inflated → inside
}

TEST(CurveMaskRegionContains, StrokeConfinesToABandAlongTheBoundary) {
    const Curve          c = blob();
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    ShapePoints region     = ShapePoints::fromCurve(c);
    region.curveMask       = static_cast<CurveMaskId>(1);
    region.strokeWidth     = 6.0f;
    EXPECT_FALSE(curveMaskRegionContains({80, 72}, region, f));  // deep inside → NOT in the band
    // A point near the boundary (the curve passes ~through (80,72)+(0,-45) = (80,27)).
    EXPECT_TRUE(curveMaskRegionContains({80, 27}, region, f));
}

TEST(CurveMaskRegionContains, TransformTranslationMovesTheRegion) {
    const Curve          c = blob();  // a wavy blob about (80,72), reach ~44 px at most
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    ShapePoints region     = ShapePoints::fromCurve(c);
    region.curveMask       = static_cast<CurveMaskId>(1);
    // Translate the region by +80 px in x: the center (80,72) interior is now reached at (160,72), which is
    // far outside the un-moved region — so the same fragment flips with the transform, proving it composes.
    ShapePoints moved = region;
    moved.transform   = Transform::translation(80, 0);
    EXPECT_TRUE(curveMaskRegionContains({80, 72}, region, f));    // center inside the un-moved region
    EXPECT_FALSE(curveMaskRegionContains({160, 72}, region, f));  // (160,72) far outside the un-moved region
    EXPECT_TRUE(curveMaskRegionContains({160, 72}, moved, f));    // and inside the translated one (maps to center)
}

TEST(CurveMaskRegionContains, InvertFlipsInsideAndOutside) {
    const Curve          c = blob();
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    ShapePoints region     = ShapePoints::fromCurve(c);
    region.curveMask       = static_cast<CurveMaskId>(1);
    EXPECT_TRUE(curveMaskRegionContains({80, 72}, region, f));
    region.invert = true;
    EXPECT_FALSE(curveMaskRegionContains({80, 72}, region, f));  // the inside is now the OUTSIDE
}

// ── bake box + clamp-to-edge addressing ─────────────────────────────────────────────────────

TEST(BakeCurveMaskField, BoxEnclosesTheCurvePlusPadding) {
    const Curve          c = blob();
    const float          padding = 8.0f;
    const CurveMaskField f = bakeCurveMaskField(c, padding, 256);
    // The blob spans x,y ∈ [35,125] about (80,72) r=45; the box min is below that minus padding.
    EXPECT_LE(f.bakeMin.x, 35.0f - padding + 0.01f);
    EXPECT_LE(f.bakeMin.y, 27.0f - padding + 0.01f);
    EXPECT_GE(f.bakeMin.x + f.bakeExtent.x, 125.0f + padding - 0.01f);
    EXPECT_GE(f.bakeMin.y + f.bakeExtent.y, 117.0f + padding - 0.01f);
}

TEST(SampleCurveMaskField, FarOutsideClampsToAPositiveDistance) {
    const Curve          c = blob();
    const CurveMaskField f = bakeCurveMaskField(c, 8.0f, 256);
    // Way outside the bake box → clamp-to-edge reads the border (a large positive = unambiguously outside).
    EXPECT_GT(sampleCurveMaskField(f, {-500, -500}), 0.0f);
    EXPECT_GT(sampleCurveMaskField(f, {900, 900}), 0.0f);
}

TEST(BakeCurveMaskField, EmptyBoundaryYieldsAnEmptyField) {
    const CurveMaskField f = bakeCurveMaskField(Curve{}, 8.0f, 256);
    EXPECT_EQ(f.width, 0);
    EXPECT_EQ(f.height, 0);
    EXPECT_TRUE(f.distances.empty());
}

// ── regionCurvePath — the renderer's routing decision ───────────────────────────────────────

TEST(RegionCurvePath, EmptyCurveIsPolygon) {
    EXPECT_EQ(regionCurvePath(ShapePoints{}), CurveRegionPath::Polygon);
    EXPECT_EQ(regionCurvePath(ShapePoints::circle({80, 72}, 30)), CurveRegionPath::Polygon);
}

TEST(RegionCurvePath, LinearQuadraticIsAnalytic) {
    Curve q;
    q.closed   = true;
    q.segments = {CurveSegment{{40, 100}, {80, 20}, {120, 100}, {}, CurveDegree::Quadratic},
                  CurveSegment{{120, 100}, {40, 100}, {}, {}, CurveDegree::Linear}};
    ShapePoints region = ShapePoints::fromCurve(q);
    region.curveMask   = static_cast<CurveMaskId>(1);  // a mask is IGNORED for an analytic boundary
    EXPECT_EQ(regionCurvePath(region), CurveRegionPath::Analytic);
}

TEST(RegionCurvePath, CubicWithMaskIsMaskWithoutIsSampledPolygon) {
    ShapePoints region = ShapePoints::fromCurve(blob());  // Catmull-Rom → cubic
    EXPECT_EQ(regionCurvePath(region), CurveRegionPath::SampledPolygon);  // no mask attached
    region.curveMask = static_cast<CurveMaskId>(1);
    EXPECT_EQ(regionCurvePath(region), CurveRegionPath::Mask);            // mask attached
}

TEST(ShapePoints, DefaultCurveMaskIsNone) {
    EXPECT_EQ(ShapePoints{}.curveMask, CurveMaskId{});
    EXPECT_EQ(static_cast<std::uint32_t>(ShapePoints{}.curveMask), 0u);
}

}  // namespace
}  // namespace retropp
