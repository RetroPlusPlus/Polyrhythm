#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/geometry.h"
#include "retropp/transform.h"

// Region-confined screen-space effects. Device-free coverage of the CPU side: the shape
// presets (draw_state.h) and the region gate the GPU region_select.frag mirrors (sdPolygon /
// regionContains / regionParams in postprocess.h). The live gate is build-compiled + dev-verified
// across all three backends (the documented CI-headless boundary); these are the failable units.

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// ── Presets — geometry ────────────────────────────────────────────────────────────────

TEST(ShapePresets, Circle) {
    const ShapePoints s = ShapePoints::circle({80, 72}, 30);
    EXPECT_EQ(s.points.size(), 1u);
    EXPECT_FLOAT_EQ(s.radius, 30.0f);
    EXPECT_EQ(s.points[0], (Point{80, 72}));
    EXPECT_TRUE(s.transform.isIdentity());
    EXPECT_TRUE(s.hasRegion());
}

TEST(ShapePresets, Capsule) {
    const ShapePoints s = ShapePoints::capsule({10, 10}, {50, 10}, 5);
    EXPECT_EQ(s.points.size(), 2u);
    EXPECT_FLOAT_EQ(s.radius, 5.0f);
    EXPECT_EQ(s.points[1], (Point{50, 10}));
}

TEST(ShapePresets, Triangle) {
    const ShapePoints s = ShapePoints::triangle({0, 0}, {10, 0}, {0, 10});
    EXPECT_EQ(s.points.size(), 3u);
    EXPECT_FLOAT_EQ(s.radius, 0.0f);
}

TEST(ShapePresets, Rectangle) {
    const ShapePoints s = ShapePoints::rectangle({20, 30}, 40, 50);
    EXPECT_EQ(s.points.size(), 4u);
    EXPECT_EQ(s.points[0], (Point{20, 30}));
    EXPECT_EQ(s.points[2], (Point{60, 80}));  // top-left + (w, h)
}

TEST(ShapePresets, RoundedRectangleIsInsetByRadius) {
    const ShapePoints s = ShapePoints::roundedRectangle({0, 0}, 100, 60, 10);
    EXPECT_EQ(s.points.size(), 4u);
    EXPECT_FLOAT_EQ(s.radius, 10.0f);
    EXPECT_EQ(s.points[0], (Point{10, 10}));  // inset by r so the inflated extent is back to w×h
    EXPECT_EQ(s.points[2], (Point{90, 50}));
}

TEST(ShapePresets, RegularPolygonSideCountUnbounded) {
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 2).points.size(), 3u);    // floored at 3
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 6).points.size(), 6u);
    EXPECT_EQ(ShapePoints::regularPolygon({0, 0}, 10, 64).points.size(), 64u);  // NO upper cap
}

TEST(ShapePresets, DefaultHasNoRegion) {
    const ShapePoints s;
    EXPECT_TRUE(s.points.empty());
    EXPECT_FALSE(s.hasRegion());
}

TEST(ScreenSpaceEffect, DefaultsToNoneAndIsRegionAgnostic) {
    const ScreenSpaceEffect e;
    EXPECT_EQ(e.kind, ScreenSpaceEffectKind::None);    // the default is no effect
    EXPECT_EQ(e.scope, ScreenSpaceEffectScope::Layer); // isolated by default
    // No effect carries geometry — confinement comes from the Region that owns it (the inversion model).
    EXPECT_EQ(e.stencil, StencilMode::TransparentInside);    // Transparency's default see-through side
    EXPECT_FLOAT_EQ(e.feather, 0.0f);                  // a hard edge by default
}

TEST(DrawState, DefaultLayerAndFrameHaveNoRegions) {
    EXPECT_TRUE(DrawLayer{.key = "x"}.regions.empty());  // regions are additive — empty by default
    EXPECT_TRUE(FrameDrawState{}.regions.empty());
}

// ── regionContains — the gate ─────────────────────────────────────────────────────────

TEST(RegionContains, NoRegionIsAlwaysInside) {
    const ShapePoints none;  // count 0 → whole viewport
    EXPECT_TRUE(regionContains({0, 0}, none));
    EXPECT_TRUE(regionContains({159, 143}, none));
}

TEST(RegionContains, CircleInsideAndOutside) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    EXPECT_TRUE(regionContains({80, 72}, c));         // center
    EXPECT_TRUE(regionContains({80, 100}, c));        // 28 px below center, inside r=30
    EXPECT_FALSE(regionContains({80, 110}, c));       // 38 px below center, outside
    EXPECT_FALSE(regionContains({80, 72 + 31}, c));   // 31 px out — just past the radius
}

TEST(RegionContains, CapsuleHugsItsSegment) {
    const ShapePoints cap = ShapePoints::capsule({40, 72}, {120, 72}, 10);
    EXPECT_TRUE(regionContains({80, 72}, cap));    // on the spine
    EXPECT_TRUE(regionContains({80, 80}, cap));    // 8 px off the spine, inside r=10
    EXPECT_FALSE(regionContains({80, 90}, cap));   // 18 px off, outside
    EXPECT_TRUE(regionContains({40, 72}, cap));    // an endpoint
    EXPECT_FALSE(regionContains({140, 72}, cap));  // 20 px past the far endpoint
}

TEST(RegionContains, RadiusInflationFlipsAPoint) {
    // A bare triangle (radius 0) excludes a point just outside the hypotenuse; inflating includes it.
    const Point probe{55, 5};
    ShapePoints t = ShapePoints::triangle({0, 0}, {50, 0}, {0, 50});
    EXPECT_FALSE(regionContains(probe, t));
    t.radius = 10.0f;
    EXPECT_TRUE(regionContains(probe, t));
}

TEST(RegionContains, RectangleBounds) {
    const ShapePoints r = ShapePoints::rectangle({20, 20}, 40, 40);  // [20,60] × [20,60]
    EXPECT_TRUE(regionContains({40, 40}, r));   // center
    EXPECT_TRUE(regionContains({21, 21}, r));   // inside near a corner
    EXPECT_FALSE(regionContains({10, 40}, r));  // left of the rect
    EXPECT_FALSE(regionContains({40, 70}, r));  // below the rect
}

TEST(RegionContains, TransformScaleEnlargesShape) {
    // A small circle that excludes a far point; scaling the region 3× about its center includes it.
    ShapePoints c = ShapePoints::circle({80, 72}, 10);
    const Point probe{80, 95};  // 23 px from center — outside r=10
    EXPECT_FALSE(regionContains(probe, c));
    c.transform = Transform::scale(3.0f, 3.0f, 80.0f, 72.0f);  // effective r ≈ 30 about the center
    EXPECT_TRUE(regionContains(probe, c));
}

TEST(RegionContains, TransformTranslationMovesShape) {
    ShapePoints c = ShapePoints::circle({40, 40}, 15);
    EXPECT_FALSE(regionContains({100, 40}, c));
    c.transform = Transform::translation(60, 0);  // shape center effectively at (100, 40)
    EXPECT_TRUE(regionContains({100, 40}, c));
}

TEST(RegionContains, UnboundedPolygonPointCount) {
    // A 64-gon (well past any former 16-cap) approximating a circle of radius 40 about (80,72).
    const ShapePoints poly = ShapePoints::regularPolygon({80, 72}, 40, 64);
    EXPECT_EQ(poly.points.size(), 64u);
    EXPECT_TRUE(regionContains({80, 72}, poly));    // center
    EXPECT_TRUE(regionContains({80, 100}, poly));   // 28 px from center, inside
    EXPECT_FALSE(regionContains({80, 120}, poly));  // 48 px from center, outside the ~40 radius
}

// A hand-built arbitrary polygon (a concave arrow) — what unbounded points unlock: a shape no preset
// produces. The notch between the barbs is OUTSIDE the shape (concavity), proving the winding SDF.
TEST(RegionContains, ConcaveArbitraryPolygon) {
    ShapePoints arrow;
    arrow.points = {{80, 20}, {120, 60}, {95, 60}, {95, 120}, {65, 120}, {65, 60}, {40, 60}};
    EXPECT_EQ(arrow.points.size(), 7u);
    EXPECT_TRUE(regionContains({80, 100}, arrow));   // in the shaft
    EXPECT_TRUE(regionContains({80, 45}, arrow));    // in the head
    EXPECT_FALSE(regionContains({45, 100}, arrow));  // beside the shaft (concave notch) — outside
    EXPECT_FALSE(regionContains({80, 130}, arrow));  // below the shaft — outside
}

// ── Viewport-grid snap (crisp evaluation) ─────────────────────────────────────────────

// The snap defaults off — regionContains(p, r) is byte-identical to the explicit unsnapped call, so every
// existing call site keeps evaluating on the Output grid.
TEST(RegionContains, SnapDefaultsToUnsnapped) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    EXPECT_EQ(regionContains({80, 101}, c), regionContains({80, 101}, c, /*snap=*/false));
}

// Snapping tests the fragment at its viewport-cell center (floor + 0.5): regionContains(frag, r, true)
// equals evaluating the pre-snapped center unsnapped — the crisp-evaluation contract.
TEST(RegionContains, SnapEvaluatesAtCellCentre) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    const Point frag{80.7f, 101.3f};
    EXPECT_EQ(regionContains(frag, c, /*snap=*/true),
              regionContains(snapFragToCellCenter(frag), c, /*snap=*/false));
}

// Teeth: a fragment can be outside on the continuous grid but inside once snapped to its cell center — the
// snap genuinely changes the evaluation point (a no-op harness would pass both settings identically).
TEST(RegionContains, SnapCanChangeContainmentNearBoundary) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 29.7f);
    const Point frag{80.0f, 101.9f};                       // 29.9 px below center → OUTSIDE on the raw grid
    EXPECT_FALSE(regionContains(frag, c, /*snap=*/false));
    EXPECT_TRUE(regionContains(frag, c, /*snap=*/true));   // cell center (80.5, 101.5) is 29.5 px → inside
}

// ── region invert — confine to the OUTSIDE of the shape ───────────────────────────────

TEST(RegionContains, InvertDefaultsToInside) {
    EXPECT_FALSE(ShapePoints::circle({80, 72}, 30).invert);  // the inside is the region by default
}

TEST(RegionContains, InvertFlipsInsideAndOutside) {
    ShapePoints c = ShapePoints::circle({80, 72}, 30);
    EXPECT_TRUE(regionContains({80, 72}, c));    // center — inside
    EXPECT_FALSE(regionContains({80, 120}, c));  // 48 px out — outside
    c.invert = true;                             // now the region is the OUTSIDE of the circle
    EXPECT_FALSE(regionContains({80, 72}, c));    // center is no longer in the region
    EXPECT_TRUE(regionContains({80, 120}, c));    // the outside now IS the region
}

TEST(RegionContains, EmptyRegionIgnoresInvert) {
    ShapePoints none;
    none.invert = true;  // no shape → no confinement; invert is moot
    EXPECT_TRUE(regionContains({0, 0}, none));
    EXPECT_TRUE(regionContains({159, 143}, none));
}

// ── ShapePoints::inverted() — the outside-targeting helper for standalone regions ──────

TEST(ShapeInverted, FlipsInsideOutsideNonDestructively) {
    const ShapePoints c   = ShapePoints::circle({80, 72}, 30);
    const ShapePoints inv = c.inverted();
    EXPECT_FALSE(c.invert);                       // the original is untouched
    EXPECT_TRUE(inv.invert);                      // the copy is flipped
    EXPECT_EQ(inv.points, c.points);              // geometry preserved
    EXPECT_FLOAT_EQ(inv.radius, c.radius);
    EXPECT_FALSE(inv.inverted().invert);          // flipping twice returns to the inside
    EXPECT_FALSE(regionContains({80, 72}, inv));  // the center is no longer in the region
    EXPECT_TRUE(regionContains({80, 120}, inv));  // the outside now IS the region
}

// ── strokeWidth — the stroke / outline shape mode (confine to the boundary band) ───────

TEST(RegionStroke, DefaultIsZeroFilledRegion) {
    EXPECT_FLOAT_EQ(ShapePoints::circle({80, 72}, 30).strokeWidth, 0.0f);  // filled by default
}

TEST(RegionStroke, CircleStrokeIsARingNotADisk) {
    ShapePoints c = ShapePoints::circle({80, 72}, 30);
    c.strokeWidth = 8.0f;                            // a ring of width 8 centered on radius 30 → dist ∈ [26,34]
    EXPECT_FALSE(regionContains({80, 72}, c));       // the center (deep interior) is NOT in the stroke
    EXPECT_TRUE(regionContains({80, 72 + 30}, c));   // on the boundary (30 px out) → in the ring
    EXPECT_TRUE(regionContains({80, 72 + 27}, c));   // 27 px out → within ±4 of the boundary
    EXPECT_TRUE(regionContains({80, 72 + 33}, c));   // 33 px out → within ±4, still in the ring
    EXPECT_FALSE(regionContains({80, 72 + 20}, c));  // 20 px out → inside the ring's inner edge → excluded
    EXPECT_FALSE(regionContains({80, 72 + 40}, c));  // 40 px out → past the ring's outer edge → excluded
}

TEST(RegionStroke, BandsAroundTheRadiusInflatedBoundary) {
    // The band rides the boundary the fill uses: a sharp triangle, stroked, bands around its edges; the
    // deep interior is excluded.
    ShapePoints t = ShapePoints::triangle({0, 0}, {100, 0}, {0, 100});
    t.strokeWidth = 6.0f;                       // band is |signedDistance| ≤ 3 about the edges
    EXPECT_FALSE(regionContains({20, 20}, t));  // deep inside the triangle → not in the stroke
    EXPECT_TRUE(regionContains({50, 0}, t));    // on the bottom edge → in the stroke band
}

TEST(RegionStroke, InvertMasksTheComplementOfTheBand) {
    ShapePoints c = ShapePoints::circle({80, 72}, 30);
    c.strokeWidth = 8.0f;
    c.invert      = true;                            // the region is everything EXCEPT the ring
    EXPECT_TRUE(regionContains({80, 72}, c));        // the center is now in the region
    EXPECT_FALSE(regionContains({80, 72 + 30}, c));  // the boundary band is excluded
}

// ── Region ownership — a region owns the effects applied inside its shape ──────────────

TEST(RegionModel, OwnsShapeAndEffectsInOrder) {
    const Region r{.key     = "r",
                   .shape   = ShapePoints::circle({80, 72}, 30),
                   .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Transparency},
                               ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple}}};
    ASSERT_EQ(r.effects.size(), 2u);
    EXPECT_EQ(r.effects[0].kind, ScreenSpaceEffectKind::Transparency);  // applied first
    EXPECT_EQ(r.effects[1].kind, ScreenSpaceEffectKind::Ripple);        // then this, in order
    EXPECT_TRUE(r.shape.hasRegion());
}

TEST(RegionModel, LayerAndFrameOwnManyRegions) {
    DrawLayer layer{.key = "layer"};
    layer.regions.push_back(Region{.key = "r0", .shape = ShapePoints::circle({40, 40}, 10)});
    layer.regions.push_back(Region{.key = "r1", .shape = ShapePoints::circle({80, 80}, 10)});
    EXPECT_EQ(layer.regions.size(), 2u);
    FrameDrawState frame;
    frame.regions.push_back(Region{.key = "fr", .shape = ShapePoints::rectangle({0, 0}, 10, 10)});
    EXPECT_EQ(frame.regions.size(), 1u);
}

// ── stencil() sugar — "make a shape see-through" expands to Regions ─────────────────────

TEST(StencilSugar, SeeThroughRegionCarriesTransparencyAtChosenScopeModeFeather) {
    const ShapePoints shape = ShapePoints::circle({80, 72}, 30);
    const std::vector<Region> regions =
        stencil(shape, StencilMode::TransparentOutside, /*feather=*/8.0f, ScreenSpaceEffectScope::Below);
    ASSERT_EQ(regions.size(), 1u);                  // see-through only — no side effects
    EXPECT_EQ(regions[0].shape.points, shape.points);  // confined to the shape
    EXPECT_FALSE(regions[0].shape.invert);          // the inside, as authored
    ASSERT_EQ(regions[0].effects.size(), 1u);
    const ScreenSpaceEffect& e = regions[0].effects[0];
    EXPECT_EQ(e.kind, ScreenSpaceEffectKind::Transparency);
    EXPECT_EQ(e.stencil, StencilMode::TransparentOutside);  // which side goes see-through
    EXPECT_FLOAT_EQ(e.feather, 8.0f);
    EXPECT_EQ(e.scope, ScreenSpaceEffectScope::Below);  // the caller's reach
}

TEST(StencilSugar, DefaultsToTransparentInsideHardLayerScope) {
    const std::vector<Region> regions = stencil(ShapePoints::circle({80, 72}, 30));
    ASSERT_EQ(regions.size(), 1u);
    const ScreenSpaceEffect& e = regions[0].effects[0];
    EXPECT_EQ(e.stencil, StencilMode::TransparentInside);
    EXPECT_FLOAT_EQ(e.feather, 0.0f);
    EXPECT_EQ(e.scope, ScreenSpaceEffectScope::Layer);
}

TEST(StencilSugar, SideEffectsBecomeBelowScopeRegionsOnTheShapeAndItsInverse) {
    const ShapePoints shape = ShapePoints::rectangle({40, 40}, 80, 64);
    const ScreenSpaceEffect ripple{.kind = ScreenSpaceEffectKind::Ripple};
    const ScreenSpaceEffect wave{.kind = ScreenSpaceEffectKind::RowDisplacement};
    const std::vector<Region> regions =
        stencil(shape, StencilMode::TransparentInside, 0.0f, ScreenSpaceEffectScope::Layer, {ripple}, {wave});
    ASSERT_EQ(regions.size(), 3u);                      // see-through + one inside + one outside
    // [1] inside effect — confined to the shape.
    EXPECT_FALSE(regions[1].shape.invert);
    ASSERT_EQ(regions[1].effects.size(), 1u);
    EXPECT_EQ(regions[1].effects[0].kind, ScreenSpaceEffectKind::Ripple);
    // The side effect is forced to Below scope — that is what makes the renderer resolve it on the composited
    // accumulator (reaching the layers showing THROUGH the see-through area), not the layer's own transparent
    // scratch. Flip this expectation to Layer and the red→green proof for reveal-then-distort fails.
    EXPECT_EQ(regions[1].effects[0].scope, ScreenSpaceEffectScope::Below);
    // [2] outside effect — confined to the INVERTED shape (the outside), also Below.
    EXPECT_TRUE(regions[2].shape.invert);
    EXPECT_EQ(regions[2].effects[0].kind, ScreenSpaceEffectKind::RowDisplacement);
    EXPECT_EQ(regions[2].effects[0].scope, ScreenSpaceEffectScope::Below);
}

TEST(StencilSugar, NoSideEffectsYieldsJustTheSeeThroughRegion) {
    const std::vector<Region> regions =
        stencil(ShapePoints::circle({80, 72}, 30), StencilMode::TransparentInside, 0.0f,
                ScreenSpaceEffectScope::Layer, {}, {});
    EXPECT_EQ(regions.size(), 1u);  // empty side lists create no extra regions
}

// ── regionParams — the cbuffer mirror ─────────────────────────────────────────────────

TEST(RegionParamsResolve, CountsRadiusAndInverseViewport) {
    const ShapePoints c = ShapePoints::circle({80, 72}, 30);
    const RegionParams p = regionParams(c, kViewport);
    EXPECT_EQ(p.count, 1u);
    EXPECT_FLOAT_EQ(p.radius, 30.0f);
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
    EXPECT_FLOAT_EQ(p.invRow0[0], 1.0f);  // identity transform → identity inverse
    EXPECT_FLOAT_EQ(p.invRow1[1], 1.0f);
    EXPECT_FLOAT_EQ(p.invRow2[2], 1.0f);
}

TEST(RegionParamsResolve, TransformInverseIsResolved) {
    ShapePoints c = ShapePoints::circle({0, 0}, 10);
    c.transform = Transform::translation(60, 0);  // inverse is translate(-60, 0)
    const RegionParams p = regionParams(c, kViewport);
    EXPECT_FLOAT_EQ(p.invRow0[2], -60.0f);
}

TEST(RegionParamsResolve, ZeroViewportYieldsZeroInverse) {
    const RegionParams p = regionParams(ShapePoints::circle({0, 0}, 1), PixelSize{0, 0});
    EXPECT_FLOAT_EQ(p.invViewportW, 0.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 0.0f);
}

TEST(RegionParamsResolve, CarriesStrokeWidth) {
    ShapePoints c = ShapePoints::circle({80, 72}, 30);
    c.strokeWidth = 5.0f;
    EXPECT_FLOAT_EQ(regionParams(c, kViewport).strokeWidth, 5.0f);                         // the shader's uInvRow1.w
    EXPECT_FLOAT_EQ(regionParams(ShapePoints::circle({80, 72}, 30), kViewport).strokeWidth, 0.0f);  // default fill
}

}  // namespace
}  // namespace retropp
