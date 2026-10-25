#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"

// Post-process composition. Device-free coverage of the CPU side: the chain-build
// helper (activeFrameEffects) and the displacement math the displace.frag GPU stage mirrors
// (displaceSourceUv / displaceParams / displacementOffset). The live ping-pong GPU path is build-
// compiled + dev-verified across all three backends (the documented CI-headless boundary); these
// are the failable units. amplitude is in viewport pixels and the sine curve is GPU-verified, so the
// assertions pin axis routing + px→UV normalization at SINE-EXACT arguments (sin(2π·0.25) = 1).

namespace retropp {
namespace {

constexpr PixelSize kViewport{160, 144};

// ── activeFrameEffects — the frame-level chain ────────────────────────────────────────

TEST(ActiveFrameEffects, EmptyByDefault) {
    const FrameDrawState frame;
    EXPECT_TRUE(activeFrameEffects(frame).empty());
}

TEST(ActiveFrameEffects, FiltersNone) {
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
    };
    const auto active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::RowDisplacement);
    EXPECT_FLOAT_EQ(active[0].amplitude, 2.0f);
}

TEST(ActiveFrameEffects, PreservesSubmissionOrder) {
    FrameDrawState frame;
    frame.postEffects = {
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 1.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f},
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 3.0f},
    };
    const auto active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 3u);
    EXPECT_FLOAT_EQ(active[0].amplitude, 1.0f);
    EXPECT_FLOAT_EQ(active[1].amplitude, 2.0f);
    EXPECT_FLOAT_EQ(active[2].amplitude, 3.0f);
}

// activeFrameEffects reads ONLY the frame-level postEffects — a per-layer DrawLayer::effect is
// invisible to it (per-layer realization is a different code path).
TEST(ActiveFrameEffects, IgnoresPerLayerEffect) {
    FrameDrawState frame;
    DrawLayer layer{.key = "layer"};
    layer.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 5.0f}};
    frame.layers.push_back(layer);
    EXPECT_TRUE(activeFrameEffects(frame).empty());
}

// ── displacementOffset — the constexpr px→UV normalization ────────────────────────────

// amplitude in viewport pixels, normalized to UV by the inverse viewport dimension, times the sine.
// Genuinely constexpr (no sin) → static_assert-testable: 80 px over a 160 px viewport at sine 1 is a
// half-UV offset.
TEST(DisplacementOffset, NormalizesPixelsToUv) {
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 80.0f, 160), 0.5f);
    EXPECT_FLOAT_EQ(displacementOffset(-1.0f, 80.0f, 160), -0.5f);
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 0.0f, 160), 0.0f);   // zero amplitude → no offset
    EXPECT_FLOAT_EQ(displacementOffset(1.0f, 80.0f, 0), 0.0f);    // degenerate viewport → no offset
    static_assert(displacementOffset(1.0f, 80.0f, 160) == 0.5f,
                  "80px over a 160px viewport at sine 1 is a half-UV offset");
}

// ── displaceSourceUv — the full mirror (axis routing + normalization) ─────────────────

// amplitude 0 → identity, regardless of axis / frequency / phase.
TEST(DisplaceSourceUv, IdentityAtZeroAmplitude) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 0.0f, .frequency = 3.0f, .phase = 0.25f};
    const Uv uv{0.4f, 0.6f};
    EXPECT_EQ(displaceSourceUv(uv, e, kViewport), uv);
}

// A None-kind effect is also identity (the chain filters these, but the mirror is defensive).
TEST(DisplaceSourceUv, IdentityForNoneKind) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::None, .amplitude = 9.0f};
    const Uv uv{0.3f, 0.7f};
    EXPECT_EQ(displaceSourceUv(uv, e, kViewport), uv);
}

// Horizontal: displaces U by sin of the ROW (v), leaves V. At frequency 1 / phase 0 / v = 0.25 the
// sine argument is 2π·0.25 → sin = 1, so U shifts by amplitude/viewportW exactly.
TEST(DisplaceSourceUv, HorizontalDisplacesUByRow) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 80.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Horizontal};
    const Uv uv{0.5f, 0.25f};
    const Uv src = displaceSourceUv(uv, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 0.5f + 0.5f);  // 80/160 · sin(π/2) = 0.5
    EXPECT_FLOAT_EQ(src.v, 0.25f);        // V untouched on the horizontal axis
}

// Vertical: displaces V by sin of the COLUMN (u), leaves U. Same sine-exact construction on the
// other axis, normalized by viewport HEIGHT (144).
TEST(DisplaceSourceUv, VerticalDisplacesVByColumn) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 72.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Vertical};
    const Uv uv{0.25f, 0.5f};
    const Uv src = displaceSourceUv(uv, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 0.25f);        // U untouched on the vertical axis
    EXPECT_FLOAT_EQ(src.v, 0.5f + 0.5f);  // 72/144 · sin(π/2) = 0.5
}

// The phase shifts the sine argument: at v = 0, frequency 1, phase 0.25 the argument is 2π·0.25 → 1.
TEST(DisplaceSourceUv, PhaseEntersTheSineArgument) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 160.0f, .frequency = 1.0f, .phase = 0.25f,
                              .axis = Axis::Horizontal};
    const Uv src = displaceSourceUv(Uv{0.0f, 0.0f}, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 1.0f);  // 160/160 · sin(2π·0.25) = 1
}

// ── Viewport-grid snap (the crisp-evaluation mirror) ──────────────────────────────────

// A coordinate maps to its viewport cell's center, whatever its position within the cell; a coordinate
// already at a cell center is a fixed point; a degenerate dimension leaves that axis alone.
TEST(SnapUvToCellCenter, MapsToCellCentreRegardlessOfPositionInCell) {
    const PixelSize vp{16, 16};
    const Uv a = snapUvToCellCenter(Uv{0.31f, 0.62f}, vp);  // 0.31·16=4.96 → cell 4; 0.62·16=9.92 → cell 9
    const Uv b = snapUvToCellCenter(Uv{0.29f, 0.60f}, vp);  // 0.29·16=4.64 → cell 4; 0.60·16=9.60 → cell 9
    EXPECT_FLOAT_EQ(a.u, 4.5f / 16.0f);
    EXPECT_FLOAT_EQ(a.v, 9.5f / 16.0f);
    EXPECT_EQ(a, b);  // same cell → same center
}

TEST(SnapUvToCellCenter, CellCentreIsAFixedPoint) {
    const PixelSize vp{16, 16};  // powers of two → the centers are exact in float
    const Uv c{5.5f / 16.0f, 9.5f / 16.0f};
    EXPECT_EQ(snapUvToCellCenter(c, vp), c);
}

TEST(SnapUvToCellCenter, DegenerateDimensionLeavesAxisUnchanged) {
    const Uv uv{0.37f, 0.62f};
    EXPECT_FLOAT_EQ(snapUvToCellCenter(uv, PixelSize{0, 16}).u, 0.37f);   // width 0 → u untouched
    EXPECT_FLOAT_EQ(snapUvToCellCenter(uv, PixelSize{16, 0}).v, 0.62f);   // height 0 → v untouched
}

// A viewport-pixel fragment moves to its cell center (floor + 0.5); a coordinate already on a center is
// unchanged.
TEST(SnapFragToCellCenter, MovesToCellCentre) {
    EXPECT_EQ(snapFragToCellCenter(Point{4.2f, 9.8f}), (Point{4.5f, 9.5f}));
    EXPECT_EQ(snapFragToCellCenter(Point{4.5f, 9.5f}), (Point{4.5f, 9.5f}));  // already a center
    EXPECT_EQ(snapFragToCellCenter(Point{-0.3f, 0.0f}), (Point{-0.5f, 0.5f})); // floor(-0.3) = -1
}

// Round-half-up: floor(v + 0.5). The .5 tie rounds UP (toward +inf), NOT to even — which is where HLSL
// round() (round-to-even) would diverge and break scale-1 parity. Negatives round toward +inf too.
TEST(RoundHalfUpPx, RoundsHalfUpNotToEven) {
    EXPECT_FLOAT_EQ(roundHalfUpPx(0.5f), 1.0f);    // round-to-even would give 0
    EXPECT_FLOAT_EQ(roundHalfUpPx(2.5f), 3.0f);    // round-to-even would give 2
    EXPECT_FLOAT_EQ(roundHalfUpPx(0.49f), 0.0f);
    EXPECT_FLOAT_EQ(roundHalfUpPx(3.2f), 3.0f);
    EXPECT_FLOAT_EQ(roundHalfUpPx(-0.5f), 0.0f);   // floor(0) = 0
    EXPECT_FLOAT_EQ(roundHalfUpPx(-1.5f), -1.0f);  // floor(-1) = -1
    EXPECT_FLOAT_EQ(roundHalfUpPx(-0.6f), -1.0f);  // floor(-0.1) = -1
}

// displaceSourceUv defaults to the Output grid (unsnapped) — the explicit false is byte-identical, so
// existing call sites are unchanged.
TEST(DisplaceSourceUv, SnapDefaultsToUnsnapped) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 5.3f, .frequency = 2.0f, .phase = 0.1f, .axis = Axis::Horizontal};
    const Uv uv{0.42f, 0.58f};
    EXPECT_EQ(displaceSourceUv(uv, e, kViewport), displaceSourceUv(uv, e, kViewport, /*snap=*/false));
}

// The snapped path evaluates the wave at the fragment's cell center and offsets by the round-half-up
// whole-pixel displacement, applied to the UNSNAPPED uv — the exact displace.frag Viewport-grid mirror.
TEST(DisplaceSourceUv, SnappedEvaluatesAtCellCentreAndQuantizes) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 4.7f, .frequency = 2.0f, .phase = 0.1f, .axis = Axis::Horizontal};
    const Uv uv{0.333f, 0.428f};
    const Uv got = displaceSourceUv(uv, e, kViewport, /*snap=*/true);
    constexpr float kTwoPi = 6.283185307179586f;
    const float evalV = snapUvToCellCenter(uv, kViewport).v;
    const float s     = std::sin(kTwoPi * (e.frequency * evalV + e.phase));
    const float expU  = uv.u + roundHalfUpPx(e.amplitude * s) / static_cast<float>(kViewport.width);
    EXPECT_FLOAT_EQ(got.u, expU);
    EXPECT_FLOAT_EQ(got.v, uv.v);  // v untouched on the horizontal axis
}

// rippleSourceUv defaults to the Output grid (unsnapped) — byte-identical to the explicit false.
TEST(RippleSourceUv, SnapDefaultsToUnsnapped) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 5.0f, .frequency = 3.0f,
                              .phase = 0.15f, .center = Point{80, 72}, .decay = 0.5f};
    const Uv uv{0.35f, 0.62f};
    EXPECT_EQ(rippleSourceUv(uv, e, kViewport), rippleSourceUv(uv, e, kViewport, /*snap=*/false));
}

// The snapped ripple evaluates at the cell center and quantizes each axis' displacement round-half-up to
// a whole viewport pixel, applied to the UNSNAPPED uv — the exact ripple.frag Viewport-grid mirror.
TEST(RippleSourceUv, SnappedEvaluatesAtCellCentreAndQuantizes) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = 5.0f, .frequency = 3.0f,
                              .phase = 0.15f, .center = Point{80, 72}, .decay = 0.5f};
    const Uv uv{0.35f, 0.62f};
    const Uv got = rippleSourceUv(uv, e, kViewport, /*snap=*/true);
    const RippleParams p = rippleParams(e, kViewport);
    constexpr float kTwoPi = 6.283185307179586f;
    const Uv    ev     = snapUvToCellCenter(uv, kViewport);
    const float dx     = ev.u - p.centerU;
    const float dy     = ev.v - p.centerV;
    const float aspect = p.invViewportW > 0.0f ? p.invViewportH / p.invViewportW : 1.0f;
    const float dist   = std::sqrt((dx * aspect) * (dx * aspect) + dy * dy);
    const float wave   = std::sin(kTwoPi * (p.frequency * dist - p.phase));
    const float env    = std::exp(-p.decay * dist);
    const float off    = p.amplitude * wave * env;
    EXPECT_FLOAT_EQ(got.u, uv.u + roundHalfUpPx((dx / dist) * off) * p.invViewportW);
    EXPECT_FLOAT_EQ(got.v, uv.v + roundHalfUpPx((dy / dist) * off) * p.invViewportH);
}

// customSampleSourceUv passes the requested coordinate through unchanged with snap off (the Output
// grid — a custom shader's sampleSource samples exactly what it asked for) and on a degenerate viewport.
TEST(CustomSampleSourceUv, PassthroughWhenUnsnappedOrDegenerate) {
    const Uv requested{0.41f, 0.73f};
    const Uv trueUv{0.40f, 0.70f};
    const Uv evalUv = snapUvToCellCenter(trueUv, kViewport);
    EXPECT_EQ(customSampleSourceUv(requested, trueUv, evalUv, kViewport, /*snap=*/false), requested);
    EXPECT_EQ(customSampleSourceUv(requested, trueUv, evalUv, PixelSize{0, 144}, /*snap=*/true), requested);
    EXPECT_EQ(customSampleSourceUv(requested, trueUv, evalUv, PixelSize{160, 0}, /*snap=*/true), requested);
}

// Snapped: the displacement the shader asked for relative to its evaluation point quantizes round-half-up
// to whole viewport pixels and applies to the fragment's TRUE uv — the preamble's sampleSource mirror.
// Ties round up (never to even) and negative offsets round toward +inf, matching roundHalfUpPx.
TEST(CustomSampleSourceUv, QuantizesTheOffsetRelativeToTheEvaluationPoint) {
    const float w = static_cast<float>(kViewport.width);
    const float h = static_cast<float>(kViewport.height);
    const Uv trueUv{0.31f, 0.62f};
    const Uv evalUv = snapUvToCellCenter(trueUv, kViewport);
    const Uv requested{evalUv.u + 2.5f / w, evalUv.v - 1.3f / h};   // +2.5 px (tie), −1.3 px
    const Uv got = customSampleSourceUv(requested, trueUv, evalUv, kViewport, /*snap=*/true);
    EXPECT_FLOAT_EQ(got.u, trueUv.u + 3.0f / w);   // roundHalfUpPx(2.5) = 3 — round-to-even would give 2
    EXPECT_FLOAT_EQ(got.v, trueUv.v - 1.0f / h);   // roundHalfUpPx(-1.3) = -1
}

// A shader that samples at its own uv (requested == the evaluation point) samples the fragment's true
// uv — a passthrough custom effect stays the identity on the Viewport grid.
TEST(CustomSampleSourceUv, IdentityRequestSamplesTheTrueUv) {
    const Uv trueUv{0.334f, 0.617f};
    const Uv evalUv = snapUvToCellCenter(trueUv, kViewport);
    EXPECT_EQ(customSampleSourceUv(evalUv, trueUv, evalUv, kViewport, /*snap=*/true), trueUv);
}

// At compose scale 1 the fragment's uv IS its cell center, and the quantized coordinate lands on the same
// viewport texel nearest sampling picks from the continuous request — the snap cannot change the scale-1
// image. Exact-tie offsets (±.5 px) are excluded: there the CONTINUOUS request sits on a texel boundary
// whose nearest pick is float-representation-dependent, while the quantized path is the defined
// round-half-up semantics (RoundHalfUpPx covers the tie itself).
TEST(CustomSampleSourceUv, Scale1TexelUnchangedAwayFromTies) {
    const float w = static_cast<float>(kViewport.width);
    const Uv centre = snapUvToCellCenter(Uv{0.42f, 0.55f}, kViewport);  // trueUv == evalUv at scale 1
    for (const float offPx : {0.0f, 0.49f, 1.7f, -0.74f, -2.3f, 3.26f}) {
        const Uv requested{centre.u + offPx / w, centre.v};
        const Uv got = customSampleSourceUv(requested, centre, centre, kViewport, /*snap=*/true);
        EXPECT_EQ(std::floor(got.u * w), std::floor(requested.u * w)) << "offset " << offPx << " px";
    }
}

// ── Edge boundary: blank (default) vs stretch ─────────────────────────────────────────

// A displaced UV inside [0,1]² samples the source; outside, it does not. The constexpr bounds
// predicate the stage's boundary branch keys off.
TEST(WithinSource, BoundsPredicate) {
    EXPECT_TRUE(withinSource(Uv{0.0f, 0.0f}));
    EXPECT_TRUE(withinSource(Uv{1.0f, 1.0f}));
    EXPECT_TRUE(withinSource(Uv{0.5f, 0.5f}));
    EXPECT_FALSE(withinSource(Uv{-0.01f, 0.5f}));  // pulled past the left edge
    EXPECT_FALSE(withinSource(Uv{1.01f, 0.5f}));   // past the right edge
    EXPECT_FALSE(withinSource(Uv{0.5f, 1.2f}));    // past the bottom
    static_assert(withinSource(Uv{0.5f, 0.5f}));
    static_assert(!withinSource(Uv{-0.1f, 0.5f}));
}

// ── bandSignedDistance — the stroke / outline shape mode ──────────────────────────────

// strokeWidth 0 returns the boundary signed distance unchanged (a filled region — the byte-identical
// default). A positive strokeWidth turns it into the signed distance to a BAND centered on the boundary
// (|d| - w/2), negative only within ±w/2 of the boundary — so the region's effects confine to the shape's
// outline. The four region/stencil shaders apply the identical transform. Pure → static_assert-testable.
TEST(BandSignedDistance, ZeroWidthIsTheFilledRegion) {
    EXPECT_FLOAT_EQ(bandSignedDistance(-5.0f, 0.0f), -5.0f);  // deep inside the fill, unchanged
    EXPECT_FLOAT_EQ(bandSignedDistance(0.0f, 0.0f), 0.0f);    // on the boundary
    EXPECT_FLOAT_EQ(bandSignedDistance(7.0f, 0.0f), 7.0f);    // outside, unchanged
    static_assert(bandSignedDistance(-5.0f, 0.0f) == -5.0f);
}

TEST(BandSignedDistance, PositiveWidthIsABandCenteredOnTheBoundary) {
    // width 4 → the band is |d| ≤ 2 around the boundary (negative = inside the band).
    EXPECT_FLOAT_EQ(bandSignedDistance(0.0f, 4.0f), -2.0f);  // on the boundary → deepest in the band
    EXPECT_LT(bandSignedDistance(-1.0f, 4.0f), 0.0f);        // just inside the fill → still in the band
    EXPECT_LT(bandSignedDistance(1.0f, 4.0f), 0.0f);         // just outside → still in the band
    EXPECT_GT(bandSignedDistance(-10.0f, 4.0f), 0.0f);       // deep in the fill interior → OUT of the band
    EXPECT_GT(bandSignedDistance(10.0f, 4.0f), 0.0f);        // far outside → out of the band
    static_assert(bandSignedDistance(0.0f, 4.0f) == -2.0f);
    static_assert(bandSignedDistance(-10.0f, 4.0f) == 8.0f);  // interior excluded from the stroke
}

TEST(BandSignedDistance, IsSymmetricAboutTheBoundary) {
    // |d| discards the sign — why an OPEN curve strokes into an open band (the containment sign is moot).
    EXPECT_FLOAT_EQ(bandSignedDistance(3.0f, 5.0f), bandSignedDistance(-3.0f, 5.0f));
    static_assert(bandSignedDistance(3.0f, 5.0f) == bandSignedDistance(-3.0f, 5.0f));
}

// The boundary decision: Blank (the default) shows the backdrop ONLY where the displaced UV left the
// source; in-bounds it samples. Stretch NEVER shows the backdrop — it samples (the CLAMP_TO_EDGE
// sampler duplicates the edge column). This is the developer-selectable behavior (default Blank).
TEST(ResolvesToBackdrop, BlankShowsBackdropOnlyOutOfBounds) {
    EXPECT_TRUE(resolvesToBackdrop(Uv{-0.05f, 0.5f}, DisplacementEdge::Blank));   // exposed strip → blank
    EXPECT_FALSE(resolvesToBackdrop(Uv{0.5f, 0.5f}, DisplacementEdge::Blank));    // in-bounds → samples
    static_assert(resolvesToBackdrop(Uv{1.5f, 0.5f}, DisplacementEdge::Blank));
}

TEST(ResolvesToBackdrop, StretchNeverShowsBackdrop) {
    EXPECT_FALSE(resolvesToBackdrop(Uv{-0.05f, 0.5f}, DisplacementEdge::Stretch));  // edge column duplicated
    EXPECT_FALSE(resolvesToBackdrop(Uv{0.5f, 0.5f}, DisplacementEdge::Stretch));
    static_assert(!resolvesToBackdrop(Uv{2.0f, 2.0f}, DisplacementEdge::Stretch));
}

// ── displaceParams — the GPU uniform's resolved values ────────────────────────────────

TEST(DisplaceParams, ResolvesInverseViewportAndAxis) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 3.0f, .frequency = 2.0f, .phase = 0.1f,
                              .axis = Axis::Vertical, .edge = DisplacementEdge::Stretch};
    const DisplaceParams p = displaceParams(e, kViewport);
    EXPECT_FLOAT_EQ(p.amplitude, 3.0f);
    EXPECT_FLOAT_EQ(p.frequency, 2.0f);
    EXPECT_FLOAT_EQ(p.phase, 0.1f);
    EXPECT_EQ(p.axis, 1u);  // Vertical
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
    EXPECT_EQ(p.edge, 1u);  // Stretch
}

TEST(DisplaceParams, EdgeDefaultsToBlankZero) {
    // A plain RowDisplacement (no edge specified) resolves to Blank (0) — the faithful default.
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_EQ(displaceParams(e, kViewport).edge, 0u);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}).edge == 0u);
}

TEST(DisplaceParams, HorizontalAxisIsZeroAndDegenerateViewportIsSafe) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .axis = Axis::Horizontal};
    const DisplaceParams p = displaceParams(e, PixelSize{0, 0});
    EXPECT_EQ(p.axis, 0u);  // Horizontal
    EXPECT_FLOAT_EQ(p.invViewportW, 0.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 0.0f);
    static_assert(displaceParams(ScreenSpaceEffect{.axis = Axis::Horizontal}, PixelSize{0, 0}).axis == 0u);
}

// ── Per-layer dispatch + scope + the transparent-blank flag ──────────────

// layerHasScreenSpaceEffect — the renderer's per-layer dispatch predicate. A default (None) effect is
// no effect, so the layer composites on the unchanged faithful path.
TEST(LayerHasScreenSpaceEffect, NoneIsNoEffect) {
    DrawLayer layer{.key = "layer"};  // default effect kind == None
    EXPECT_FALSE(layerHasScreenSpaceEffect(layer));
}

TEST(LayerHasScreenSpaceEffect, RowDisplacementIsAnEffect) {
    DrawLayer layer{.key = "layer"};
    layer.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement}};
    EXPECT_TRUE(layerHasScreenSpaceEffect(layer));
}

// The effects member is a CHAIN: a layer carrying several effects scans true if any is a real effect, and
// an all-None chain is no effect (so the layer stays on the faithful path).
TEST(LayerHasScreenSpaceEffect, ChainScansAnyRealEffect) {
    DrawLayer layer{.key = "layer"};
    layer.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
                     ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple}};
    EXPECT_TRUE(layerHasScreenSpaceEffect(layer));
    layer.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None}};
    EXPECT_FALSE(layerHasScreenSpaceEffect(layer));
}

// Effect scope defaults to Layer (isolated — displace only this layer); Below is the adjustment-layer
// scope (displace the whole accumulator at this z). The renderer routes the two differently.
TEST(EffectScope, DefaultsToLayerIsolated) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement};
    EXPECT_EQ(e.scope, ScreenSpaceEffectScope::Layer);
    EXPECT_FALSE(effectIsBelowScope(e));
    static_assert(!effectIsBelowScope(ScreenSpaceEffect{}));
}

TEST(EffectScope, BelowRoutesAsAdjustmentLayer) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .scope = ScreenSpaceEffectScope::Below};
    EXPECT_TRUE(effectIsBelowScope(e));
    static_assert(effectIsBelowScope(ScreenSpaceEffect{.scope = ScreenSpaceEffectScope::Below}));
}

// displaceParams.blankTransparent — the scope-dependent blank color selector. Frame-level + Below
// pass false (opaque backdrop, the C.2.a default); the isolated Layer pass passes true (transparent,
// so the exposed strip reveals the layers below).
TEST(DisplaceParams, BlankTransparentDefaultsOpaque) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f};
    EXPECT_EQ(displaceParams(e, kViewport).blankTransparent, 0u);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}).blankTransparent == 0u);
}

TEST(DisplaceParams, BlankTransparentTrueForIsolatedLayer) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 2.0f};
    const DisplaceParams p = displaceParams(e, kViewport, /*blankTransparent=*/true);
    EXPECT_EQ(p.blankTransparent, 1u);
    // The flag does not disturb the other resolved params.
    EXPECT_FLOAT_EQ(p.amplitude, 2.0f);
    EXPECT_EQ(p.axis, 0u);
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    static_assert(displaceParams(ScreenSpaceEffect{}, PixelSize{160, 144}, true).blankTransparent == 1u);
}

// The per-layer realization drives the SAME displacement math as the frame-level path — a regression
// guard that scope changes the renderer's routing, not the displacement curve.
TEST(DisplaceSourceUv, PerLayerEffectUsesTheSameMirror) {
    const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::RowDisplacement,
                              .amplitude = 80.0f, .frequency = 1.0f, .phase = 0.0f,
                              .axis = Axis::Horizontal, .scope = ScreenSpaceEffectScope::Layer};
    const Uv src = displaceSourceUv(Uv{0.5f, 0.25f}, e, kViewport);
    EXPECT_FLOAT_EQ(src.u, 1.0f);   // 0.5 + 80/160·sin(π/2)
    EXPECT_FLOAT_EQ(src.v, 0.25f);
}

}  // namespace
}  // namespace retropp
