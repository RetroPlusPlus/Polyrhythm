// Sprite anchors, origin/pivot placement, and within-layer z-order.
//
// Anchors are ART-space named points that ride the texture orientation ops (orientPoint — the
// continuous forward inverse of sourceCellTexel's dest→source read) and resolve through the sprite's
// transform + placement (anchor(k, Space::Quad) → quad space, anchor(k, Space::Layer) → layer space).
// The origin is the QUAD-space
// placement handle Sprite::x/y place; the pivot is the QUAD-space point the geometric transform spins
// about. makeGpuSprite bakes the chain so a local quad point p lands at pos + (pivot − origin) + T·(p −
// pivot); at identity that cancels to pos + (p − origin), so a pivot change never moves an untransformed
// sprite. Sprite::z stacks sprites within their layer: spriteDrawOrder sorts ascending, stable on ties.
// All headless — the CPU mirrors of what the GPU rasterizes, plus the interpolator's origin/pivot easing.

#include "retropp/draw_state.h"
#include "retropp/interpolation.h"
#include "steady_timing.h"
#include "retropp/transform.h"

#include <cmath>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace retropp {
namespace {

constexpr float kTol = 1e-4f;

// ── orientPoint: the continuous art→quad map ─────────────────────────────────────────────────

TEST(OrientPoint, IdentityLeavesPointsInPlace) {
    constexpr Point p = orientPoint(Point{2.5f, 3.5f}, 8, 8, Rotation::None, false, false);
    static_assert(p == Point{2.5f, 3.5f});
    EXPECT_EQ(p, (Point{2.5f, 3.5f}));
}

TEST(OrientPoint, FlipsMirrorAboutTheCellExtent) {
    EXPECT_EQ(orientPoint(Point{2.5f, 3.5f}, 8, 8, Rotation::None, true, false), (Point{5.5f, 3.5f}));
    EXPECT_EQ(orientPoint(Point{2.5f, 3.5f}, 8, 8, Rotation::None, false, true), (Point{2.5f, 4.5f}));
    EXPECT_EQ(orientPoint(Point{2.5f, 3.5f}, 8, 8, Rotation::None, true, true), (Point{5.5f, 4.5f}));
}

TEST(OrientPoint, RotationsPlaceArtCorners) {
    // The art's top-left pixel center (0.5, 0.5) of an 8×8 cell under each rotation (CW).
    EXPECT_EQ(orientPoint(Point{0.5f, 0.5f}, 8, 8, Rotation::Rot90, false, false), (Point{7.5f, 0.5f}));
    EXPECT_EQ(orientPoint(Point{0.5f, 0.5f}, 8, 8, Rotation::Rot180, false, false), (Point{7.5f, 7.5f}));
    EXPECT_EQ(orientPoint(Point{0.5f, 0.5f}, 8, 8, Rotation::Rot270, false, false), (Point{0.5f, 7.5f}));
}

// The correspondence proof: for every destination pixel of a cell, sourceCellTexel names the ART pixel
// it reads — so orientPoint must map that art pixel's CENTER back into that destination pixel, for all
// eight orientations. Square and non-square (the Rot90/270 transpose quirk) both hold.
TEST(OrientPoint, AgreesWithSourceCellTexelAtPixelCentresAllOrientations) {
    constexpr int w = 8, h = 4;  // non-square on purpose; the loop also runs the square case
    for (const auto [cw, ch] : {std::pair{8, 8}, std::pair{w, h}}) {
        for (int rot = 0; rot < 4; ++rot) {
            for (int flip = 0; flip < 4; ++flip) {
                const auto r  = static_cast<Rotation>(rot);
                const bool fx = (flip & 1) != 0;
                const bool fy = (flip & 2) != 0;
                for (int dy = 0; dy < ch; ++dy) {
                    for (int dx = 0; dx < cw; ++dx) {
                        const CellTexel src = sourceCellTexel(dx, dy, cw, ch, r, fx, fy);
                        const Point dest = orientPoint(
                            Point{static_cast<float>(src.x) + 0.5f, static_cast<float>(src.y) + 0.5f},
                            cw, ch, r, fx, fy);
                        EXPECT_EQ(static_cast<int>(std::floor(dest.x)), dx)
                            << "cell " << cw << "x" << ch << " rot=" << rot << " flip=" << flip
                            << " dest px (" << dx << "," << dy << ")";
                        EXPECT_EQ(static_cast<int>(std::floor(dest.y)), dy)
                            << "cell " << cw << "x" << ch << " rot=" << rot << " flip=" << flip
                            << " dest px (" << dx << "," << dy << ")";
                    }
                }
            }
        }
    }
}

// ── Anchor tables ────────────────────────────────────────────────────────────────────────────

constexpr Anchor kClawAnchors[] = {
    {.label = "hinge",  .x = 2.0f, .y = 3.0f},
    {.label = "tip",    .x = 7.0f, .y = 1.0f},
    {.label = "socket", .x = 0.0f, .y = 6.0f},
};
static_assert(!findDuplicateAnchorLabel(kClawAnchors), "claw anchor labels must be unique");

TEST(AnchorTable, DuplicateLabelIsFoundAndUniqueTablePasses) {
    constexpr Anchor dup[] = {
        {.label = "a", .x = 1.0f, .y = 1.0f},
        {.label = "b", .x = 2.0f, .y = 2.0f},
        {.label = "a", .x = 3.0f, .y = 3.0f},
    };
    static_assert(findDuplicateAnchorLabel(dup).value() == "a");
    EXPECT_EQ(findDuplicateAnchorLabel(dup).value(), "a");
    EXPECT_FALSE(findDuplicateAnchorLabel(kClawAnchors).has_value());
    EXPECT_FALSE(findDuplicateAnchorLabel({}).has_value());
}

// ── anchor(k, Space::Quad): label + index addressing, orientation riding, loud misses ──────────

TEST(AnchorQuad, LabelAndIndexAgreeOnTheUntransformedSprite) {
    const Sprite s{.key = "claw", .anchors = kClawAnchors};
    EXPECT_EQ(s.anchor("hinge", Space::Quad), (Point{2.0f, 3.0f}));
    EXPECT_EQ(s.anchor(std::size_t{0}, Space::Quad), (Point{2.0f, 3.0f}));
    EXPECT_EQ(s.anchor("socket", Space::Quad), s.anchor(std::size_t{2}, Space::Quad));
}

TEST(AnchorQuad, FlipMirrorsTheAnchorWithTheArt) {
    Sprite s{.key = "claw", .anchors = kClawAnchors};
    s.flipX = true;  // 8×8 default cell: x = 8 − 2 = 6
    EXPECT_EQ(s.anchor("hinge", Space::Quad), (Point{6.0f, 3.0f}));
    s.flipY = true;
    EXPECT_EQ(s.anchor("hinge", Space::Quad), (Point{6.0f, 5.0f}));
}

TEST(AnchorQuad, RotationCarriesTheAnchorWithTheArt) {
    Sprite s{.key = "claw", .anchors = kClawAnchors};
    s.rotation = Rotation::Rot90;  // (x, y) → (w − y, x) on the 8×8 default cell
    EXPECT_EQ(s.anchor("hinge", Space::Quad), (Point{5.0f, 2.0f}));
}

TEST(AnchorQuad, UnknownLabelAndOutOfRangeIndexThrow) {
    const Sprite s{.key = "claw", .anchors = kClawAnchors};
    EXPECT_THROW((void)s.anchor("no-such-socket", Space::Quad), std::out_of_range);
    EXPECT_THROW((void)s.anchor(std::size_t{3}, Space::Quad), std::out_of_range);
    const Sprite bare{.key = "bare"};  // no anchor table at all
    EXPECT_THROW((void)bare.anchor("hinge", Space::Layer), std::out_of_range);
}

TEST(AnchorQuad, DuplicateLabelResolvesToTheFirstMatch) {
    static constexpr Anchor dup[] = {
        {.label = "a", .x = 1.0f, .y = 1.0f},
        {.label = "a", .x = 9.0f, .y = 9.0f},
    };
    const Sprite s{.key = "dup", .anchors = dup};
    EXPECT_EQ(s.anchor("a", Space::Quad), (Point{1.0f, 1.0f}));
}

// ── anchor(k, Space::Layer) / toLayer: placement + transform composition ────────────────────────

TEST(AnchorLayer, IdentityTransformPlacesByTheOrigin) {
    Sprite s{.key = "claw", .x = 100, .y = 50, .anchors = kClawAnchors};
    // Default origin {0,0}: x/y place the top-left, so the layer-space anchor = pos + local.
    EXPECT_EQ(s.anchor("hinge", Space::Layer), (Point{102.0f, 53.0f}));
    // Mount by the hinge — origin = pivot = the hinge makes THAT point sit at (x, y).
    s.pivot  = Point{2.0f, 3.0f};
    s.origin = Point{2.0f, 3.0f};
    EXPECT_EQ(s.anchor("hinge", Space::Layer), (Point{100.0f, 50.0f}));  // the hinge is the mount point
    EXPECT_EQ(s.anchor("tip", Space::Layer), (Point{105.0f, 48.0f}));    // pos + (tip − hinge)
}

TEST(AnchorLayer, OriginFixingTransformKeepsTheMountAnchoredAtPosition) {
    // Mount by the hinge (origin = pivot = hinge): the hinge is both placement handle and spin center,
    // so it stays at (x, y) under any origin-fixing transform.
    Sprite s{.key = "claw", .x = 40, .y = 60, .pivot = Point{2.0f, 3.0f}, .origin = Point{2.0f, 3.0f},
             .anchors = kClawAnchors};
    for (const float deg : {0.0f, 30.0f, 90.0f, 137.0f, 270.0f}) {
        s.transform = Transform::rotation(deg);
        const Point hinge = s.anchor("hinge", Space::Layer);  // the hinge sits ON the mount
        EXPECT_NEAR(hinge.x, 40.0f, kTol) << "deg=" << deg;
        EXPECT_NEAR(hinge.y, 60.0f, kTol) << "deg=" << deg;
    }
}

TEST(AnchorLayer, RotationSweepsAnAnchorAroundTheMount) {
    Sprite s{.key = "claw", .x = 40, .y = 60, .pivot = Point{2.0f, 3.0f}, .origin = Point{2.0f, 3.0f},
             .anchors = kClawAnchors};
    s.transform = Transform::rotation(90.0f);  // CW in top-left-origin pixel space
    // tip − pivot = (5, −2); rotated 90° CW → (2, 5); pos + that = (42, 65).
    const Point tip = s.anchor("tip", Space::Layer);
    EXPECT_NEAR(tip.x, 42.0f, kTol);
    EXPECT_NEAR(tip.y, 65.0f, kTol);
}

// The Layer-space anchor is exactly toLayer of the Quad-space anchor — the space argument selects
// between the raw quad point and that point mapped through transform + placement.
TEST(AnchorLayer, IsToLayerOfTheQuadAnchor) {
    Sprite s{.key = "claw", .x = 12, .y = 34, .pivot = Point{2.0f, 3.0f}, .origin = Point{1.0f, 5.0f},
             .anchors = kClawAnchors};
    s.transform = Transform::rotation(50.0f);
    for (const std::string_view label : {"hinge", "tip", "socket"}) {
        const Point expect = s.toLayer(s.anchor(label, Space::Quad));
        const Point got    = s.anchor(label, Space::Layer);
        EXPECT_NEAR(got.x, expect.x, kTol) << label;
        EXPECT_NEAR(got.y, expect.y, kTol) << label;
    }
}

// Origin and pivot decoupled: x/y place the origin, the transform spins about the pivot, and the pivot's
// own image is the fixed point pos + (pivot − origin), invariant of the angle.
TEST(AnchorLayer, OriginAndPivotDecoupledFixedPoint) {
    Sprite s{.key = "claw", .x = 40, .y = 60, .pivot = Point{2.0f, 3.0f}, .origin = Point{5.0f, 1.0f},
             .anchors = kClawAnchors};
    for (const float deg : {0.0f, 45.0f, 90.0f, 200.0f}) {
        s.transform = Transform::rotation(deg);
        // pos + (pivot − origin) = (40,60) + (2−5, 3−1) = (37, 62), whatever the angle.
        const Point atPivot = s.toLayer(s.pivot);
        EXPECT_NEAR(atPivot.x, 37.0f, kTol) << "deg=" << deg;
        EXPECT_NEAR(atPivot.y, 62.0f, kTol) << "deg=" << deg;
    }
}

TEST(ToLayer, MapsAnyQuadPointThroughTransformAndPlacement) {
    Sprite s{.key = "q", .x = 10, .y = 20, .pivot = Point{4.0f, 4.0f}, .origin = Point{1.0f, 1.0f}};
    s.transform = Transform::scale(2.0f, 2.0f);
    // world(p) = pos + (pivot − origin) + T·(p − pivot)
    //          = (10,20) + (3,3) + 2·(4,−4) = (13,23) + (8,−8) = (21, 15).
    EXPECT_EQ(s.toLayer(Point{8.0f, 0.0f}), (Point{21.0f, 15.0f}));
    // The pivot's image is the fixed point pos + (pivot − origin).
    EXPECT_EQ(s.toLayer(s.pivot), (Point{13.0f, 23.0f}));
}

// At identity the pivot drops out and x/y place the origin: world(p) = pos + (p − origin).
TEST(ToLayer, IdentityCancelsThePivotAndPlacesTheOrigin) {
    Sprite s{.key = "q", .x = 10, .y = 20, .pivot = Point{4.0f, 6.0f}, .origin = Point{3.0f, 5.0f}};
    EXPECT_EQ(s.toLayer(s.origin), (Point{10.0f, 20.0f}));          // the origin lands at pos
    EXPECT_EQ(s.toLayer(Point{7.0f, 9.0f}), (Point{14.0f, 24.0f})); // pos + (7,9) − (3,5)
    // Changing the pivot cannot move an untransformed sprite.
    s.pivot = Point{1.0f, 2.0f};
    EXPECT_EQ(s.toLayer(Point{7.0f, 9.0f}), (Point{14.0f, 24.0f}));
}

TEST(SpriteCenter, QuadSpaceIsTheHalfExtent) {
    EXPECT_EQ((Sprite{.key = "a", .size = AssetDimensions{16, 16}}.center(Space::Quad)), (Point{8.0f, 8.0f}));
    EXPECT_EQ((Sprite{.key = "b", .size = AssetDimensions{16, 8}}.center(Space::Quad)), (Point{8.0f, 4.0f}));
    EXPECT_EQ((Sprite{.key = "c"}.center(Space::Quad)), (Point{4.0f, 4.0f}));  // default 8×8 cell
}

// With no transform and default placement the Layer center equals the Quad center shifted by position.
TEST(SpriteCenter, LayerSpaceIsTheQuadCentreThroughPlacement) {
    const Sprite s{.key = "a", .x = 100, .y = 40, .size = AssetDimensions{16, 16}};
    EXPECT_EQ(s.center(Space::Layer), (Point{108.0f, 48.0f}));  // pos + {8,8}, default origin {0,0}
}

// Under a transform the Layer center resolves THROUGH transform + placement (toLayer of the Quad
// center) and differs from the Quad center — the space argument is not cosmetic.
TEST(SpriteCenter, LayerSpaceResolvesThroughTheTransform) {
    Sprite s{.key = "a", .x = 30, .y = 30, .size = AssetDimensions{16, 16},
             .pivot = Point{0.0f, 0.0f}, .origin = Point{0.0f, 0.0f}};
    s.transform = Transform::scale(2.0f, 2.0f);
    // toLayer({8,8}) = pos + (pivot − origin) + T·(center − pivot) = (30,30) + (0,0) + 2·(8,8) = (46,46).
    EXPECT_EQ(s.center(Space::Layer), (Point{46.0f, 46.0f}));
    EXPECT_EQ(s.center(Space::Layer), s.toLayer(s.center(Space::Quad)));
    EXPECT_NE(s.center(Space::Layer), s.center(Space::Quad));
}

// ── makeGpuSprite: origin/pivot in the baked chain ───────────────────────────────────────────

// Reconstruct the baked forward homography from a GpuSprite's rows.
[[nodiscard]] constexpr Transform homographyOf(const GpuSprite& g) noexcept {
    return Transform{g.row0[0], g.row0[1], g.row0[2],
                     g.row1[0], g.row1[1], g.row1[2],
                     g.row2[0], g.row2[1], g.row2[2]};
}

// The default origin AND pivot compose OUT of the chain, bit-for-bit: the re-anchor translation and the
// (pivot − origin) offset are both the exact identity matrix, so the baked homography equals the same
// chain written without them — the property the committed goldens pin.
TEST(GpuSpritePivot, DefaultOriginAndPivotComposeOutOfTheChainExactly) {
    Sprite s{.key = "p", .x = 23, .y = 41, .size = AssetDimensions{16, 16}};
    s.transform = Transform::rotation(33.0f, 8.0f, 8.0f);
    const Transform layerT = Transform::skew(0.2f, 0.0f, 10.0f, 5.0f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 3.0f, 7.0f, layerT);

    // The chain without the re-anchor step: scale → transform → translate(scrolled pos) → layer → clip.
    const float sox = static_cast<float>(s.x) - 3.0f;
    const float soy = static_cast<float>(s.y) - 7.0f;
    const Transform screenToClip{2.0f / 160.0f, 0.0f, -1.0f,
                                 0.0f, -2.0f / 144.0f, 1.0f,
                                 0.0f, 0.0f, 1.0f};
    const Transform unpivoted = Transform::scale(16.0f, 16.0f)
                                    .then(s.transform)
                                    .then(Transform::translation(sox, soy))
                                    .then(layerT);

    // Rotated sprite on the Viewport grid → analytic (inflated) — compare on the Output grid, where H
    // is the exact forward map.
    const GpuSprite exact = makeGpuSprite(s, 160, 144, 3.0f, 7.0f, layerT, EvaluationGrid::Output);
    EXPECT_EQ(homographyOf(exact), unpivoted.then(screenToClip));

    // And the analytic record's stored INVERSE is exactly the un-pivoted S⁻¹ (the un-inflated map).
    const Transform sinv = unpivoted.inverse();
    EXPECT_EQ(g.inv0[0], sinv.m00);
    EXPECT_EQ(g.inv0[2], sinv.m02);
    EXPECT_EQ(g.inv2[2], sinv.m22);
}

// A quad corner travels the baked H to exactly where toLayer says the matching local point lands
// (identity layer, zero scroll: clip → screen must reproduce the CPU resolver).
TEST(GpuSpritePivot, BakedChainAgreesWithToLayerAtTheQuadCorners) {
    Sprite s{.key = "p", .x = 30, .y = 40, .size = AssetDimensions{16, 8},
             .pivot = Point{5.0f, 2.0f}, .origin = Point{1.0f, 1.0f}};
    s.transform = Transform::rotation(25.0f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f, Transform{}, EvaluationGrid::Output);
    const Transform H = homographyOf(g);

    const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (const auto& c : corners) {
        const float clipX = H.applyX(c[0], c[1]);
        const float clipY = H.applyY(c[0], c[1]);
        const float sx = (clipX + 1.0f) * 0.5f * 160.0f;   // clip → screen (viewport px)
        const float sy = (1.0f - clipY) * 0.5f * 144.0f;
        const Point expect = s.toLayer(Point{c[0] * 16.0f, c[1] * 8.0f});
        EXPECT_NEAR(sx, expect.x, kTol) << "corner (" << c[0] << "," << c[1] << ")";
        EXPECT_NEAR(sy, expect.y, kTol) << "corner (" << c[0] << "," << c[1] << ")";
    }
}

// With an identity transform the origin is a pure placement shift (the quad's top-left lands at
// pos − origin, still an axis-aligned rect — no analytic flag), and the pivot is a no-op.
TEST(GpuSpritePivot, IdentityTransformOriginIsAPureShiftPivotIsANoOp) {
    Sprite s{.key = "p", .x = 30, .y = 40, .origin = Point{4.0f, 6.0f}};
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    EXPECT_EQ(g.flags & kSpriteAnalyticFlag, 0u);
    const Transform H = homographyOf(g);
    const float sx = (H.applyX(0.0f, 0.0f) + 1.0f) * 0.5f * 160.0f;
    const float sy = (1.0f - H.applyY(0.0f, 0.0f)) * 0.5f * 144.0f;
    EXPECT_NEAR(sx, 26.0f, kTol);  // 30 − 4
    EXPECT_NEAR(sy, 34.0f, kTol);  // 40 − 6

    // Adding a pivot with no transform moves nothing — same top-left.
    s.pivot = Point{2.0f, 5.0f};
    const GpuSprite gp = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    const Transform Hp = homographyOf(gp);
    EXPECT_NEAR((Hp.applyX(0.0f, 0.0f) + 1.0f) * 0.5f * 160.0f, 26.0f, kTol);
    EXPECT_NEAR((1.0f - Hp.applyY(0.0f, 0.0f)) * 0.5f * 144.0f, 34.0f, kTol);
}

// ── spriteDrawOrder: within-layer stacking ───────────────────────────────────────────────────

TEST(SpriteDrawOrder, SortsAscendingByZ) {
    const std::vector<Sprite> sprites{
        {.key = "front", .z = 5},
        {.key = "back", .z = -2},
        {.key = "mid", .z = 0},
    };
    const auto order = spriteDrawOrder(sprites);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1u);  // back  (z = −2)
    EXPECT_EQ(order[1], 2u);  // mid   (z =  0)
    EXPECT_EQ(order[2], 0u);  // front (z =  5)
}

TEST(SpriteDrawOrder, EqualZKeepsSubmissionOrder) {
    const std::vector<Sprite> sprites{
        {.key = "a", .z = 1},
        {.key = "b", .z = 0},
        {.key = "c", .z = 1},
        {.key = "d", .z = 0},
        {.key = "e", .z = 1},
    };
    const auto order = spriteDrawOrder(sprites);
    const std::vector<std::size_t> expect{1u, 3u, 0u, 2u, 4u};  // b, d then a, c, e — ties stable
    EXPECT_EQ(order, expect);
}

TEST(SpriteDrawOrder, AllDefaultZIsTheIdentityOrder) {
    const std::vector<Sprite> sprites{{.key = "a"}, {.key = "b"}, {.key = "c"}};
    const auto order = spriteDrawOrder(sprites);
    const std::vector<std::size_t> expect{0u, 1u, 2u};
    EXPECT_EQ(order, expect);
}

TEST(SpriteDrawOrder, EmptySpanYieldsEmptyOrder) {
    EXPECT_TRUE(spriteDrawOrder({}).empty());
}

// ── Interpolation: the pivot eases; z snaps ──────────────────────────────────────────────────

FrameDrawState frameWithSprite(std::vector<Sprite>& storage, Sprite s) {
    storage.clear();
    storage.push_back(std::move(s));
    FrameDrawState frame;
    frame.layers.push_back(DrawLayer{
        .key = "layer", .z = 0, .content = SpriteContent{std::span<const Sprite>(storage.data(), 1)}});
    return frame;
}

TEST(InterpolatorPivot, MountsSnappedThenEasesBetweenTicks) {
    Interpolator interp;
    std::vector<Sprite> storage;

    Sprite a{.key = "s", .x = 0, .y = 0, .pivot = Point{0.0f, 0.0f}, .origin = Point{0.0f, 0.0f}};
    FrameDrawState fa = frameWithSprite(storage, a);
    interp.reconcile(fa, tickAt(0.0f));
    ASSERT_TRUE(interp.spriteCur("s").has_value());
    EXPECT_EQ(interp.spriteCur("s")->pivot, (Point{0.0f, 0.0f}));

    Sprite b{.key = "s", .x = 0, .y = 0, .pivot = Point{4.0f, 8.0f}, .origin = Point{2.0f, 6.0f}};
    FrameDrawState fb = frameWithSprite(storage, b);
    interp.reconcile(fb, tickAt(0.0f));
    EXPECT_EQ(interp.spritePrev("s")->pivot, (Point{0.0f, 0.0f}));
    EXPECT_EQ(interp.spriteCur("s")->pivot, (Point{4.0f, 8.0f}));

    const FrameDrawState& mid = interp.interpolate(fb, tickAt(0.5f));
    const auto& sprites = std::get<SpriteContent>(mid.layers[0].content).sprites;
    ASSERT_EQ(sprites.size(), 1u);
    EXPECT_EQ(sprites[0].pivot, (Point{2.0f, 4.0f}));   // pivot eases {0,0} → {4,8}
    EXPECT_EQ(sprites[0].origin, (Point{1.0f, 3.0f}));  // origin eases {0,0} → {2,6}
}

TEST(InterpolatorPivot, ZIsDiscreteAndSnapsToTheSubmission) {
    Interpolator interp;
    std::vector<Sprite> storage;

    FrameDrawState fa = frameWithSprite(storage, Sprite{.key = "s", .z = 1});
    interp.reconcile(fa, tickAt(0.0f));
    FrameDrawState fb = frameWithSprite(storage, Sprite{.key = "s", .z = 7});
    interp.reconcile(fb, tickAt(0.0f));

    const FrameDrawState& mid = interp.interpolate(fb, tickAt(0.5f));
    const auto& sprites = std::get<SpriteContent>(mid.layers[0].content).sprites;
    ASSERT_EQ(sprites.size(), 1u);
    EXPECT_EQ(sprites[0].z, 7);  // the submission's z, not an eased value
}

TEST(InterpolatorPivot, PivotChangeFlagsTheSpriteChanged) {
    Interpolator interp;
    std::vector<Sprite> storage;

    FrameDrawState fa = frameWithSprite(storage, Sprite{.key = "s", .pivot = Point{1.0f, 1.0f}});
    interp.reconcile(fa, tickAt(0.0f));
    FrameDrawState fb = frameWithSprite(storage, Sprite{.key = "s", .pivot = Point{1.0f, 1.0f}});
    interp.reconcile(fb, tickAt(0.0f));
    EXPECT_FALSE(interp.spriteChanged("s"));

    FrameDrawState fc = frameWithSprite(storage, Sprite{.key = "s", .pivot = Point{2.0f, 1.0f}});
    interp.reconcile(fc, tickAt(0.0f));
    EXPECT_TRUE(interp.spriteChanged("s"));
}

}  // namespace
}  // namespace retropp
