// Per-sprite + per-layer geometric transform on the SPRITES path.
//
// makeGpuSprite bakes a COMPOSED clip-space homography into each GpuSprite: a unit-quad corner
// (cx, cy) ∈ {0,1}² travels scale(w,h) → Sprite::transform → translate(scrolled top-left) →
// DrawLayer::transform → screen→clip. The vertex shader then evaluates clip = H · (cx,cy,1) and
// the GPU perspective-divides. These headless tests reconstruct H from the baked rows and evaluate
// it at the unit corners — the exact placement the GPU computes — so the bake is verified without a
// device. The cornerstone is the IDENTITY case: with no sprite/layer transform the corners must
// reproduce the axis-aligned (clipX + cx·clipW, clipY + cy·clipH) rect bit-for-bit.

#include "retropp/draw_state.h"
#include "retropp/transform.h"

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

namespace retropp {
namespace {

// Reconstruct the baked homography from a GpuSprite's three rows (the matrix the vertex shader
// applies). Pure + constexpr so identity-case assertions can fold at compile time.
[[nodiscard]] constexpr Transform homographyOf(const GpuSprite& g) noexcept {
    return Transform{g.row0[0], g.row0[1], g.row0[2],
                     g.row1[0], g.row1[1], g.row1[2],
                     g.row2[0], g.row2[1], g.row2[2]};
}

constexpr float kTol = 1e-4f;

// ── The cornerstone: identity transforms reproduce the axis-aligned quad bit-for-bit ──

TEST(SpriteTransform, IdentityReproducesLegacyAxisAlignedQuad) {
    Sprite s{.key = "s"};
    s.x = 40;
    s.y = 24;
    s.size = AssetDimensions{16, 16};
    // viewport 160×144, scroll (8, 4) → screen top-left (32, 20). The axis-aligned bake would have
    // produced clipX = 32/160*2-1, clipW = 16/160*2, clipY = 1-20/144*2, clipH = -(16/144*2).
    const GpuSprite g = makeGpuSprite(s, 160, 144, 8, 4);  // no layer transform (defaulted)
    const Transform H = homographyOf(g);

    const float clipX = 32.0f / 160.0f * 2.0f - 1.0f;
    const float clipW = 16.0f / 160.0f * 2.0f;
    const float clipY = 1.0f - 20.0f / 144.0f * 2.0f;
    const float clipH = -(16.0f / 144.0f * 2.0f);

    // Every unit corner maps to the rect's corresponding corner, w ≡ 1 (affine).
    EXPECT_NEAR(H.applyX(0.0f, 0.0f), clipX,          kTol);
    EXPECT_NEAR(H.applyY(0.0f, 0.0f), clipY,          kTol);
    EXPECT_NEAR(H.applyX(1.0f, 0.0f), clipX + clipW,  kTol);
    EXPECT_NEAR(H.applyY(1.0f, 0.0f), clipY,          kTol);
    EXPECT_NEAR(H.applyX(0.0f, 1.0f), clipX,          kTol);
    EXPECT_NEAR(H.applyY(0.0f, 1.0f), clipY + clipH,  kTol);
    EXPECT_NEAR(H.applyX(1.0f, 1.0f), clipX + clipW,  kTol);
    EXPECT_NEAR(H.applyY(1.0f, 1.0f), clipY + clipH,  kTol);
    EXPECT_NEAR(H.weight(0.5f, 0.5f), 1.0f,           kTol);
}

TEST(SpriteTransform, IdentityProducesConstantAffineBottomRow) {
    // A Sprite owns its key (a std::string), so it is not a literal type — makeGpuSprite runs at runtime.
    const Sprite s{.key = "s"};
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0, 0);
    // Affine identity case: bottom row is (0,0,1), so w is constant 1.
    EXPECT_FLOAT_EQ(g.row2[0], 0.0f);
    EXPECT_FLOAT_EQ(g.row2[1], 0.0f);
    EXPECT_FLOAT_EQ(g.row2[2], 1.0f);
}

// ── Per-sprite transform ──────────────────────────────────────────────────────────────

TEST(SpriteTransform, PerSpriteTranslationOffsetsTheQuad) {
    Sprite base{.key = "s"};
    base.x = 0;
    base.y = 0;
    base.size = AssetDimensions{8, 8};

    Sprite moved = base;
    moved.transform = Transform::translation(8.0f, 0.0f);  // shift one sprite-width right, sprite-local

    // viewport 16×16: 8 sprite-local px = 8 screen px = +1.0 in clip x. Output grid → the pure forward
    // map (no crisp inflation), so the bake is asserted directly.
    const Transform a = homographyOf(makeGpuSprite(base,  16, 16, 0, 0, Transform{}, EvaluationGrid::Output));
    const Transform b = homographyOf(makeGpuSprite(moved, 16, 16, 0, 0, Transform{}, EvaluationGrid::Output));
    EXPECT_NEAR(b.applyX(0.0f, 0.0f) - a.applyX(0.0f, 0.0f), 1.0f, kTol);  // 8/16*2
    EXPECT_NEAR(b.applyY(0.0f, 0.0f) - a.applyY(0.0f, 0.0f), 0.0f, kTol);
}

TEST(SpriteTransform, PerSpriteScaleAboutCentre) {
    Sprite s{.key = "s"};
    s.x = 0;
    s.y = 0;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::scale(2.0f, 2.0f, 4.0f, 4.0f);  // 2× about the sprite center (4,4)

    // Scaling about the center keeps the center fixed and doubles the half-extent. viewport 16×16.
    // Output grid → the exact forward map (crisp inflation is asserted separately, below).
    const Transform H = homographyOf(makeGpuSprite(s, 16, 16, 0, 0, Transform{}, EvaluationGrid::Output));
    // Center unit (0.5,0.5) → sprite-local (4,4) → fixed point of the scale → screen (4,4) → clip.
    const float centreClipX = 4.0f / 16.0f * 2.0f - 1.0f;  // -0.5
    const float centreClipY = 1.0f - 4.0f / 16.0f * 2.0f;  //  0.5
    EXPECT_NEAR(H.applyX(0.5f, 0.5f), centreClipX, kTol);
    EXPECT_NEAR(H.applyY(0.5f, 0.5f), centreClipY, kTol);
    // Top-left corner moves out to sprite-local (-4,-4) → screen (-4,-4).
    EXPECT_NEAR(H.applyX(0.0f, 0.0f), -4.0f / 16.0f * 2.0f - 1.0f, kTol);  // -1.5
    EXPECT_NEAR(H.applyY(0.0f, 0.0f), 1.0f + 4.0f / 16.0f * 2.0f,  kTol);  //  1.5
}

TEST(SpriteTransform, PerSpriteRotation90AboutCentre) {
    Sprite s{.key = "s"};
    s.x = 0;
    s.y = 0;
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::rotation(90.0f, 4.0f, 4.0f);  // 90° CW about the sprite center

    // Worked by hand: rotation(90) about (4,4) maps sprite-local (x,y) → (8 - y, x); screen→clip
    // with vw=vh=16 is x'=x/8-1, y'=1-y/8. Unit(0,0)→local(0,0)→(8,0)→clip(0,1). Output grid → exact map.
    const Transform H = homographyOf(makeGpuSprite(s, 16, 16, 0, 0, Transform{}, EvaluationGrid::Output));
    EXPECT_NEAR(H.applyX(0.0f, 0.0f),  0.0f, kTol);  // local (8,0)
    EXPECT_NEAR(H.applyY(0.0f, 0.0f),  1.0f, kTol);
    EXPECT_NEAR(H.applyX(1.0f, 0.0f),  0.0f, kTol);  // local (8,8)
    EXPECT_NEAR(H.applyY(1.0f, 0.0f),  0.0f, kTol);
    EXPECT_NEAR(H.applyX(0.0f, 1.0f), -1.0f, kTol);  // local (0,0)
    EXPECT_NEAR(H.applyY(0.0f, 1.0f),  1.0f, kTol);
    // The center is invariant under a rotation about itself.
    EXPECT_NEAR(H.applyX(0.5f, 0.5f), -0.5f, kTol);
    EXPECT_NEAR(H.applyY(0.5f, 0.5f),  0.5f, kTol);
}

// ── Per-layer transform reaches sprites ─────────────────────────────────────────────────

TEST(SpriteTransform, PerLayerTransformAloneMovesAllCorners) {
    Sprite s{.key = "s"};  // identity sprite at origin, 8×8
    s.size = AssetDimensions{8, 8};
    const Transform layer = Transform::translation(16.0f, 0.0f);  // shift the whole layer +16 screen px

    const Transform plain   = homographyOf(makeGpuSprite(s, 32, 32, 0, 0, Transform{}, EvaluationGrid::Output));
    const Transform shifted = homographyOf(makeGpuSprite(s, 32, 32, 0, 0, layer, EvaluationGrid::Output));
    // 16 screen px on a 32px viewport = +1.0 clip x, applied to every corner identically.
    for (float cx : {0.0f, 1.0f}) {
        for (float cy : {0.0f, 1.0f}) {
            EXPECT_NEAR(shifted.applyX(cx, cy) - plain.applyX(cx, cy), 1.0f, kTol);
            EXPECT_NEAR(shifted.applyY(cx, cy) - plain.applyY(cx, cy), 0.0f, kTol);
        }
    }
}

TEST(SpriteTransform, SpriteThenLayerComposeInOrder) {
    Sprite s{.key = "s"};
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::translation(8.0f, 0.0f);          // sprite-local: +8 px right
    const Transform layer = Transform::scale(2.0f, 1.0f, 0.0f, 0.0f);  // layer: 2× horizontally about screen x=0

    // The sprite offset happens in sprite-local space, THEN the layer doubles it in screen space:
    // a +8px sprite-local shift at screen origin becomes +16 screen px after the layer 2× → +1.0
    // clip x on a 32px viewport. Order matters: layer-then-sprite would give +8px → +0.5.
    const Transform plain = homographyOf(makeGpuSprite(Sprite{.key = "s", .size = AssetDimensions{8, 8}},
                                                       32, 32, 0, 0, layer, EvaluationGrid::Output));
    const Transform both  = homographyOf(makeGpuSprite(s, 32, 32, 0, 0, layer, EvaluationGrid::Output));
    EXPECT_NEAR(both.applyX(0.0f, 0.0f) - plain.applyX(0.0f, 0.0f), 1.0f, kTol);
}

// ── Projective support survives the bake ────────────────────────────────────────────────

TEST(SpriteTransform, PerspectiveYieldsVaryingW) {
    Sprite s{.key = "s"};
    s.size = AssetDimensions{8, 8};
    s.transform = Transform::perspective(0.05f, 0.0f);  // foreshorten along sprite-local x

    const Transform H = homographyOf(makeGpuSprite(s, 64, 64, 0, 0, Transform{}, EvaluationGrid::Output));
    // A perspective transform makes the homogeneous w vary across the quad (w ≢ 1) — the property an
    // affine "origin + two edge vectors" representation could NOT carry.
    const float wNear = H.weight(0.0f, 0.0f);  // sprite-local x=0
    const float wFar  = H.weight(1.0f, 0.0f);  // sprite-local x=8 → w grew by 0.05*8
    EXPECT_NEAR(wNear, 1.0f, kTol);
    EXPECT_NEAR(wFar,  1.4f, kTol);            // 1 + 0.05*8
    EXPECT_GT(wFar, wNear + 0.1f);             // genuinely projective, not affine
}

// ── Scroll subtraction + field passthrough ──────────────────────────────────────────────

TEST(SpriteTransform, ScrollShiftsTheQuadBeforeTheLayerTransform) {
    Sprite s{.key = "s"};
    s.size = AssetDimensions{8, 8};
    const Transform a = homographyOf(makeGpuSprite(s, 32, 32, 0,  0));
    const Transform b = homographyOf(makeGpuSprite(s, 32, 32, 16, 0));  // scroll right 16
    // Scrolling the layer right moves the sprite left in screen space: -16 px on a 32px viewport = -1.0 clip x.
    EXPECT_NEAR(b.applyX(0.0f, 0.0f) - a.applyX(0.0f, 0.0f), -1.0f, kTol);
}

TEST(SpriteTransform, AttrFieldsPassThroughUnchangedByTransforms) {
    Sprite s{.key = "s"};
    s.size = AssetDimensions{16, 16};
    s.tile = 0x00AB;
    s.atlas = static_cast<AtlasId>(3);
    s.palette = static_cast<PaletteId>(7);
    s.flipX = true;
    s.flipY = true;
    s.transform = Transform::rotation(33.0f, 8.0f, 8.0f);  // arbitrary transform must not touch attr

    // Output grid → the sprite is NOT analytic, so flags carry only the flip/rotation bits (the analytic
    // bit's own coverage is asserted in the Viewport-grid cases below).
    const GpuSprite g = makeGpuSprite(s, 64, 64, 0, 0, Transform::scale(3.0f, 3.0f), EvaluationGrid::Output);
    EXPECT_EQ(g.tile, 0x00ABu);
    EXPECT_EQ(g.atlasPalette,
              packSpriteAtlasPalette(static_cast<AtlasId>(3), static_cast<PaletteId>(7)));
    EXPECT_EQ(g.flags, packSpriteFlags(true, true));  // flip bits independent of geometry
    EXPECT_EQ(g.size, (16u << 16) | 16u);
}

// ── Crisp coverage: the analytic (Viewport-grid) record ─────────────────────────────────

// Reconstruct the stored screen→unit inverse from a GpuSprite's inv rows.
[[nodiscard]] constexpr Transform inverseOf(const GpuSprite& g) noexcept {
    return Transform{g.inv0[0], g.inv0[1], g.inv0[2],
                     g.inv1[0], g.inv1[1], g.inv1[2],
                     g.inv2[0], g.inv2[1], g.inv2[2]};
}

// The un-inflated unit→viewport map S the record's inverse should invert, built the same way
// makeGpuSprite builds it (scale → sprite transform → scrolled translation → layer transform).
[[nodiscard]] Transform unitToViewport(const Sprite& s, float sox, float soy, const Transform& layer) {
    return Transform::scale(static_cast<float>(s.size.width), static_cast<float>(s.size.height))
        .then(s.transform)
        .then(Transform::translation(sox, soy))
        .then(layer);
}

TEST(SpriteInverse, StoredInverseRoundtripsScreenToUnit) {
    Sprite s{.key = "s"};
    s.x = 20;
    s.y = 12;
    s.size = AssetDimensions{16, 16};
    s.transform = Transform::rotation(37.0f, 8.0f, 8.0f);
    const Transform layer = Transform::scale(1.5f, 0.75f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 4, 2, layer);  // integer overload: scroll (4,2)

    const Transform S   = unitToViewport(s, 20.0f - 4.0f, 12.0f - 2.0f, layer);
    const Transform inv = inverseOf(g);
    // The inverse maps a screen point produced by S back to the unit coordinate that produced it.
    for (float u : {0.0f, 0.5f, 1.0f}) {
        for (float v : {0.0f, 0.5f, 1.0f}) {
            const float sx = S.applyX(u, v), sy = S.applyY(u, v);
            EXPECT_NEAR(inv.applyX(sx, sy), u, 1e-3f);
            EXPECT_NEAR(inv.applyY(sx, sy), v, 1e-3f);
        }
    }
}

TEST(SpriteAnalytic, FlagSetOnlyForTransformedSpriteOnViewportGrid) {
    // Identity sprite + identity layer → the cheap plain path on either grid.
    Sprite plain{.key = "s"};
    plain.size = AssetDimensions{8, 8};
    EXPECT_EQ(makeGpuSprite(plain, 64, 64, 0, 0).flags & kSpriteAnalyticFlag, 0u);
    EXPECT_EQ(makeGpuSprite(plain, 64, 64, 0, 0, Transform{}, EvaluationGrid::Output).flags
                  & kSpriteAnalyticFlag, 0u);

    // A per-sprite transform → analytic on Viewport, off on Output.
    Sprite tr{.key = "s"};
    tr.size = AssetDimensions{8, 8};
    tr.transform = Transform::rotation(20.0f, 4.0f, 4.0f);
    EXPECT_NE(makeGpuSprite(tr, 64, 64, 0, 0).flags & kSpriteAnalyticFlag, 0u);
    EXPECT_EQ(makeGpuSprite(tr, 64, 64, 0, 0, Transform{}, EvaluationGrid::Output).flags
                  & kSpriteAnalyticFlag, 0u);

    // A per-LAYER transform alone also triggers the analytic path on Viewport.
    Sprite id{.key = "s"};
    id.size = AssetDimensions{8, 8};
    const Transform layer = Transform::scale(2.0f, 2.0f);
    EXPECT_NE(makeGpuSprite(id, 64, 64, 0, 0, layer).flags & kSpriteAnalyticFlag, 0u);
    EXPECT_EQ(makeGpuSprite(id, 64, 64, 0, 0, layer, EvaluationGrid::Output).flags
                  & kSpriteAnalyticFlag, 0u);
}

TEST(SpriteAnalytic, OutputGridForwardMapIsExactComposedHomography) {
    // The Output-grid record for a transformed sprite is the exact composed forward map: no
    // inflation, no analytic bit.
    Sprite s{.key = "s"};
    s.size = AssetDimensions{16, 16};
    s.transform = Transform::scale(2.0f, 1.5f, 8.0f, 8.0f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0, 0, Transform{}, EvaluationGrid::Output);
    EXPECT_EQ(g.flags & kSpriteAnalyticFlag, 0u);

    const Transform screenToClip{2.0f / 160.0f, 0.0f, -1.0f,
                                 0.0f, -2.0f / 144.0f, 1.0f,
                                 0.0f, 0.0f, 1.0f};
    const Transform H = unitToViewport(s, 0.0f, 0.0f, Transform{}).then(screenToClip);
    const Transform got = homographyOf(g);
    EXPECT_FLOAT_EQ(got.m00, H.m00); EXPECT_FLOAT_EQ(got.m01, H.m01); EXPECT_FLOAT_EQ(got.m02, H.m02);
    EXPECT_FLOAT_EQ(got.m10, H.m10); EXPECT_FLOAT_EQ(got.m11, H.m11); EXPECT_FLOAT_EQ(got.m12, H.m12);
    EXPECT_FLOAT_EQ(got.m20, H.m20); EXPECT_FLOAT_EQ(got.m21, H.m21); EXPECT_FLOAT_EQ(got.m22, H.m22);
}

TEST(SpriteInflation, StepByEpsilonCoversAtLeastTheMargin) {
    // For an affine map the corner-Jacobian bound is exactly conservative: a step of εu in unit-u (εv in
    // v) must displace the screen point by at least the 1px margin — the coverage guarantee.
    const Transform S = Transform::scale(16.0f, 16.0f)
                            .then(Transform::rotation(30.0f, 8.0f, 8.0f))
                            .then(Transform::translation(20.0f, 10.0f));
    const detail::SpriteInflation infl = detail::spriteInflation(S, 1.0f);
    ASSERT_TRUE(infl.ok);
    EXPECT_GT(infl.eu, 0.0f);
    EXPECT_GT(infl.ev, 0.0f);
    for (float u : {0.0f, 1.0f}) {
        for (float v : {0.0f, 1.0f}) {
            const float bx = S.applyX(u, v), by = S.applyY(u, v);
            const float ux = S.applyX(u + infl.eu, v), uy = S.applyY(u + infl.eu, v);
            const float vx = S.applyX(u, v + infl.ev), vy = S.applyY(u, v + infl.ev);
            EXPECT_GE(std::hypot(ux - bx, uy - by), 1.0f - 1e-3f);
            EXPECT_GE(std::hypot(vx - bx, vy - by), 1.0f - 1e-3f);
        }
    }
}

TEST(SpriteAnalytic, DegenerateTransformFallsBackToSmoothPath) {
    // A per-sprite perspective that pushes a unit corner behind the projection: analytic must clear and
    // the forward map must equal the Output-grid map exactly — the documented boundary.
    Sprite s{.key = "s"};
    s.size = AssetDimensions{16, 16};
    s.transform = Transform::perspective(0.0f, -0.2f);  // weight ≤ 0 at the far edge
    const GpuSprite gv = makeGpuSprite(s, 160, 144, 0, 0);  // Viewport (default)
    EXPECT_EQ(gv.flags & kSpriteAnalyticFlag, 0u);

    const GpuSprite go = makeGpuSprite(s, 160, 144, 0, 0, Transform{}, EvaluationGrid::Output);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(gv.row0[i], go.row0[i]);
        EXPECT_FLOAT_EQ(gv.row1[i], go.row1[i]);
        EXPECT_FLOAT_EQ(gv.row2[i], go.row2[i]);
    }
}

// ── sampleSpriteCell: the CPU mirror of the fragment's analytic coverage branch ─────────────

TEST(SampleSpriteCell, HalfOpenCoverageAndTrailingClamp) {
    // A 2× scale about the origin: unit (u,v) → viewport (32u, 32v) for a 16×16 sprite. The inverse maps
    // a viewport point back to unit; sampleSpriteCell snaps to the cell center internally.
    const Transform S   = Transform::scale(16.0f, 16.0f).then(Transform::scale(2.0f, 2.0f));
    const Transform inv = S.inverse();

    const SpriteCellSample a = sampleSpriteCell(inv, 1.5f, 1.5f, 16, 16);   // unit ~0.047 → texel 0
    EXPECT_TRUE(a.covered);
    EXPECT_EQ(a.px, 0);
    EXPECT_EQ(a.py, 0);

    const SpriteCellSample b = sampleSpriteCell(inv, 31.5f, 31.5f, 16, 16); // unit ~0.984 → texel 15
    EXPECT_TRUE(b.covered);
    EXPECT_EQ(b.px, 15);
    EXPECT_EQ(b.py, 15);

    // Half-open: a cell center at unit ≥ 1 (viewport 32.5 → u ~1.016) is NOT covered.
    EXPECT_FALSE(sampleSpriteCell(inv, 32.5f, 16.0f, 16, 16).covered);
    // A cell center left of the quad (negative unit) is not covered.
    EXPECT_FALSE(sampleSpriteCell(inv, -0.5f, 16.0f, 16, 16).covered);
}

TEST(SampleSpriteCell, PerspectiveWeightBehindProjectionDiscards) {
    // An inverse whose bottom row drives the perspective weight non-positive at a cell center → discard,
    // never a divide-by-zero or a wild texel.
    const Transform inv{1.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f,
                        0.0f, -0.1f, 1.0f};  // cw = 1 − 0.1·cy; cy = 15.5 → cw = −0.55
    EXPECT_FALSE(sampleSpriteCell(inv, 4.0f, 15.0f, 16, 16).covered);
    // A near-origin cell center keeps a positive weight AND maps inside the unit quad → covered:
    // cx = cy = 0.5, cw = 0.95, u = v = 0.5/0.95 ≈ 0.526 ∈ [0,1).
    EXPECT_TRUE(sampleSpriteCell(inv, 0.0f, 0.0f, 16, 16).covered);
}

// ── Origin / pivot placement in the baked chain ──────────────────────────────────────────────

// The origin is the placement handle: under an identity transform the corner (0,0) lands at pos − origin,
// with the (pivot − origin) term baked into the forward map H.
TEST(SpriteTransform, OriginShiftsPlacementInH) {
    Sprite s{.key = "s", .x = 50, .y = 30, .size = AssetDimensions{16, 16}, .origin = Point{4.0f, 6.0f}};
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    const Transform H = homographyOf(g);
    const float sx = (H.applyX(0.0f, 0.0f) + 1.0f) * 0.5f * 160.0f;
    const float sy = (1.0f - H.applyY(0.0f, 0.0f)) * 0.5f * 144.0f;
    EXPECT_NEAR(sx, 46.0f, kTol);  // 50 − 4
    EXPECT_NEAR(sy, 24.0f, kTol);  // 30 − 6
}

// The stored inverse round-trips the S map with origin and pivot decoupled: forward through H, back to
// unit space through Sinv, returns the corner.
TEST(SpriteTransform, StoredInverseRoundTripsWithOriginAndPivot) {
    Sprite s{.key = "s", .x = 20, .y = 12, .size = AssetDimensions{16, 8},
             .pivot = Point{5.0f, 2.0f}, .origin = Point{1.0f, 7.0f}};
    s.transform = Transform::rotation(40.0f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f, Transform{}, EvaluationGrid::Output);
    const Transform H = homographyOf(g);
    const Transform Sinv{g.inv0[0], g.inv0[1], g.inv0[2],
                         g.inv1[0], g.inv1[1], g.inv1[2],
                         g.inv2[0], g.inv2[1], g.inv2[2]};
    const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (const auto& c : corners) {
        const float sx = (H.applyX(c[0], c[1]) + 1.0f) * 0.5f * 160.0f;  // clip → viewport px
        const float sy = (1.0f - H.applyY(c[0], c[1])) * 0.5f * 144.0f;
        EXPECT_NEAR(Sinv.applyX(sx, sy), c[0], kTol) << "corner (" << c[0] << "," << c[1] << ")";
        EXPECT_NEAR(Sinv.applyY(sx, sy), c[1], kTol) << "corner (" << c[0] << "," << c[1] << ")";
    }
}

// A sprite that sets origin and pivot explicitly to {0,0} bakes byte-identically to one that sets neither
// — the default pair composes out of the chain, leaving the record bit-equal.
TEST(SpriteTransform, ExplicitDefaultOriginPivotMatchesUnset) {
    Sprite unset{.key = "s", .x = 33, .y = 21, .size = AssetDimensions{16, 16}};
    unset.transform = Transform::rotation(18.0f);
    Sprite paired = unset;
    paired.pivot  = Point{0.0f, 0.0f};
    paired.origin = Point{0.0f, 0.0f};
    const GpuSprite a = makeGpuSprite(unset,  160, 144, 4.0f, 2.0f);
    const GpuSprite b = makeGpuSprite(paired, 160, 144, 4.0f, 2.0f);
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(GpuSprite)), 0);
}

}  // namespace
}  // namespace retropp
