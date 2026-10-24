// The Bloom effect's device-free CPU authorities: the resolved parameters (bloomParams), the brightpass
// (applyBrightpass), the Gaussian kernel (gaussianKernelWeight + the invNorm normalization), the additive
// composite (applyBloomAdd), the sprite reach + flags (spriteRadialReach / makeGpuSprite), and the sprite
// record path (packSpriteFxRecord lanes; evalSpriteFxRecords passes a Bloom record through — the
// art-neighborhood sum itself runs in the sprite fragment, whose pure pieces these are). A composed
// mini-oracle sums a synthetic art the way the fragment's kernel loop does and pins the glow's shape.

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

namespace retropp {
namespace {

// ── bloomParams — the resolved kernel + knobs ─────────────────────────────────────────────

TEST(BloomParamsResolve, NormalizesKnobsAndDerivesKernel) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom};
    e.radius = 4.0f; e.threshold = 128; e.intensity = 255;
    const BloomParams p = bloomParams(e);
    EXPECT_FLOAT_EQ(p.radius, 4.0f);
    EXPECT_EQ(p.taps, 4);
    EXPECT_NEAR(p.threshold, 128.0f / 255.0f, 1e-6f);
    EXPECT_FLOAT_EQ(p.intensity, 1.0f);
    // invNorm is 1/Σw — multiplying the kernel sum by it lands at 1.
    float sum = 0.0f;
    for (int k = -p.taps; k <= p.taps; ++k) sum += gaussianKernelWeight(k, p.radius);
    EXPECT_NEAR(sum * p.invNorm, 1.0f, 1e-5f);
}

TEST(BloomParamsResolve, FractionalRadiusCeilsAndCapsAtThirtyTwo) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom};
    e.radius = 2.3f;
    EXPECT_EQ(bloomParams(e).taps, 3);      // ⌈2.3⌉
    e.radius = 100.0f;
    EXPECT_EQ(bloomParams(e).taps, 32);     // the loop-bound clamp
    e.radius = -5.0f;
    const BloomParams p = bloomParams(e);   // a negative radius is no reach
    EXPECT_FLOAT_EQ(p.radius, 0.0f);
    EXPECT_EQ(p.taps, 0);
}

TEST(BloomParamsResolve, DefaultsAreIdentity) {
    const BloomParams p = bloomParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom});
    EXPECT_FLOAT_EQ(p.radius, 0.0f);
    EXPECT_EQ(p.taps, 0);
    EXPECT_FLOAT_EQ(p.threshold, 0.0f);
    EXPECT_FLOAT_EQ(p.intensity, 0.0f);     // the identity default: no glow
}

// ── gaussianKernelWeight — the Gaussian shape ────────────────────────────────────────────────

TEST(BloomKernel, CentredSymmetricAndDecreasing) {
    EXPECT_FLOAT_EQ(gaussianKernelWeight(0, 6.0f), 1.0f);                       // exp(0)
    EXPECT_FLOAT_EQ(gaussianKernelWeight(3, 6.0f), gaussianKernelWeight(-3, 6.0f));  // symmetric
    EXPECT_GT(gaussianKernelWeight(1, 6.0f), gaussianKernelWeight(2, 6.0f));       // monotone outward
    EXPECT_GT(gaussianKernelWeight(2, 6.0f), gaussianKernelWeight(5, 6.0f));
}

// ── applyBrightpass — the luminance floor ─────────────────────────────────────────────────

TEST(Brightpass, ScalesByLuminanceAboveThreshold) {
    // threshold 0: f = saturate(lum) — white passes whole, black contributes nothing.
    constexpr Vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    constexpr Vec4 black{0.0f, 0.0f, 0.0f, 1.0f};
    static_assert(applyBrightpass(white, 0.0f) == white, "white passes the brightpass whole");
    static_assert(applyBrightpass(black, 0.0f) == Vec4{0.0f, 0.0f, 0.0f, 0.0f},
                  "black contributes no glow (its alpha scales away with it)");
    // A mid pixel scales by its own luminance — dark content glows less.
    const Vec4 mid = applyBrightpass(Vec4{0.5f, 0.5f, 0.5f, 1.0f}, 0.0f);
    EXPECT_NEAR(mid.x, 0.25f, 1e-5f);   // 0.5 · lum(0.5)
    EXPECT_NEAR(mid.w, 0.5f, 1e-5f);
}

TEST(Brightpass, ThresholdFloorsAndRescales) {
    // Below the floor: nothing passes.
    const Vec4 dim = applyBrightpass(Vec4{0.3f, 0.3f, 0.3f, 1.0f}, 0.5f);
    EXPECT_FLOAT_EQ(dim.x, 0.0f);
    EXPECT_FLOAT_EQ(dim.w, 0.0f);
    // Above the floor: the survivor rescales toward full strength — white at any threshold passes whole.
    const Vec4 hot = applyBrightpass(Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.5f);
    EXPECT_FLOAT_EQ(hot.x, 1.0f);
    // A threshold at the top of the range keeps the divisor finite (the 1/255 clamp).
    const Vec4 top = applyBrightpass(Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_LE(top.x, 1.0f);
}

// ── applyBloomAdd — the additive composite ────────────────────────────────────────────────

TEST(BloomAdd, IntensityZeroIsExactIdentity) {
    constexpr Vec4 src{0.3f, 0.5f, 0.7f, 0.6f};
    constexpr Vec4 glow{0.9f, 0.9f, 0.9f, 0.9f};
    static_assert(applyBloomAdd(src, glow, 0.0f) == src, "intensity 0 adds nothing — byte-exact identity");
}

TEST(BloomAdd, AddsLightAndLiftsCoverage) {
    const Vec4 out = applyBloomAdd(Vec4{0.2f, 0.2f, 0.2f, 0.5f}, Vec4{0.4f, 0.3f, 0.2f, 0.6f}, 1.0f);
    EXPECT_NEAR(out.x, 0.6f, 1e-6f);                 // src + glow
    EXPECT_NEAR(out.w, 0.5f + 0.6f * 0.5f, 1e-6f);   // src.a + glow.a·(1 − src.a)
    // Opaque content stays opaque; the rgb sum is unclamped (float16 headroom to the blit).
    const Vec4 hot = applyBloomAdd(Vec4{0.9f, 0.9f, 0.9f, 1.0f}, Vec4{0.8f, 0.8f, 0.8f, 1.0f}, 1.0f);
    EXPECT_NEAR(hot.x, 1.7f, 1e-6f);
    EXPECT_FLOAT_EQ(hot.w, 1.0f);
}

// ── The composed glow oracle — the fragment's kernel loop over a synthetic art ────────────

// One bright texel in a transparent 5×5 field: the glow at a neighbor decays with distance and is zero
// beyond the tap extent — the halo's shape, composed from the same pure pieces the fragment loops over.
TEST(BloomGlowOracle, LoneBrightTexelRadiatesAndFadesOut) {
    constexpr int   kSide   = 5;
    constexpr float kRadius = 2.0f;
    const BloomParams p = [] {
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom};
        e.radius = kRadius; e.intensity = 255;
        return bloomParams(e);
    }();
    std::array<Vec4, kSide * kSide> art{};       // transparent everywhere…
    art[2 * kSide + 2] = Vec4{1.0f, 1.0f, 1.0f, 1.0f};  // …except the center (premultiplied white)

    auto glowAt = [&](int px, int py) {
        Vec4 g{};
        for (int dy = -p.taps; dy <= p.taps; ++dy)
            for (int dx = -p.taps; dx <= p.taps; ++dx) {
                const int ax = px + dx, ay = py + dy;
                if (ax < 0 || ax >= kSide || ay < 0 || ay >= kSide) continue;  // off-art: transparent
                const float w = gaussianKernelWeight(dx, p.radius) * gaussianKernelWeight(dy, p.radius);
                const Vec4  b = applyBrightpass(art[static_cast<std::size_t>(ay) * kSide + ax], p.threshold);
                g.x += w * b.x; g.y += w * b.y; g.z += w * b.z; g.w += w * b.w;
            }
        const float n2 = p.invNorm * p.invNorm;
        return Vec4{g.x * n2, g.y * n2, g.z * n2, g.w * n2};
    };

    const float atCentre = glowAt(2, 2).w;
    const float oneOff   = glowAt(3, 2).w;
    const float twoOff   = glowAt(4, 2).w;
    EXPECT_GT(atCentre, oneOff);        // the halo decays outward
    EXPECT_GT(oneOff, twoOff);
    EXPECT_GT(twoOff, 0.0f);            // still lit at the tap extent (the bright texel is K away)
    // Beyond the kernel reach nothing accumulates: from (2,2), every point with an axis distance > K
    // gathers zero — the same walk with the bright texel out of every tap's range.
    Vec4 far{};
    for (int dy = -p.taps; dy <= p.taps; ++dy)
        for (int dx = -p.taps; dx <= p.taps; ++dx)
            if (5 + dx == 2 && 2 + dy == 2) far.w += 1.0f;  // a tap from x = 5 never lands on x = 2 with K = 2
    EXPECT_FLOAT_EQ(far.w, 0.0f);
}

// ── Sprite emission steps ─────────────────────────────────────────────────────────────────

TEST(BloomSpriteEmission, LayerScopeStepEmitsBelowScopeDoesNot) {
    Sprite s{.key = "b"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 5.0f, .intensity = 255}};
    const auto steps = spriteEmissionSteps(s, 1.0f);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_FALSE(steps[0].glow);              // Bloom radiates the source's own light
    EXPECT_FLOAT_EQ(steps[0].reach, 5.0f);

    Sprite below{.key = "b2"};
    below.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom,
                                       .scope = ScreenSpaceEffectScope::Below, .radius = 5.0f,
                                       .intensity = 255}};
    EXPECT_TRUE(spriteEmissionSteps(below, 1.0f).empty());  // a scene lens is the below pass, not a raster

    // Two Blooms at different reaches are two emissions, not one at the wider reach — each halo is its own.
    Sprite two{.key = "b3"};
    two.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 3.0f, .intensity = 255},
                   ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 7.0f, .intensity = 255}};
    const auto both = spriteEmissionSteps(two, 1.0f);
    ASSERT_EQ(both.size(), 2u);
    EXPECT_FLOAT_EQ(both[0].reach, 3.0f);
    EXPECT_FLOAT_EQ(both[1].reach, 7.0f);
    EXPECT_EQ(groupSpriteEmissionBuckets(both).size(), 2u);
    // spriteDisplaceBound is displacement-only; a halo contributes nothing to the render footprint.
    EXPECT_FLOAT_EQ(detail::spriteDisplaceBound(two).u, 0.0f);
}

TEST(BloomSpriteEmission, ZeroReachStillEmitsWhereGlowWouldNot) {
    // A Bloom at zero reach still lays its own un-blurred brightpass back over the source, so it engages;
    // a Glow with no reach has no aura at all. The per-kind gate the frame chain uses holds here too.
    Sprite b{.key = "b"};
    b.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 0.0f, .intensity = 255}};
    EXPECT_EQ(spriteEmissionSteps(b, 1.0f).size(), 1u);

    Sprite g{.key = "g"};
    g.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 0.0f, .intensity = 255}};
    EXPECT_TRUE(spriteEmissionSteps(g, 1.0f).empty());
}

TEST(BloomSpriteEmission, TheDisplacementBoundSurvivesTheReachDeletion) {
    // A displacing sprite inflates its footprint by the displacement bound and goes analytic. An aura on
    // the same sprite adds nothing to either — it rasters and blurs in buffer space.
    Sprite s{.key = "wave", .x = 10, .y = 10};
    s.size    = AssetDimensions{16, 16};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 9.0f, .intensity = 255}};
    EXPECT_FLOAT_EQ(detail::spriteDisplaceBound(s).u, 4.0f);
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    EXPECT_NE(g.flags & kSpriteAnalyticFlag, 0u);
    EXPECT_NE(g.flags & kSpriteHasDisplacementFlag, 0u);

    // The inflation is the displacement's alone: 4 art px of 16 = ¼ of a quad on each side, so the forward
    // map's u scale is 1 + 2·¼ = 1.5× the un-inflated one. The 9 px aura contributes nothing.
    Sprite bare{.key = "wave2", .x = 10, .y = 10};
    bare.size    = AssetDimensions{16, 16};
    bare.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::RowDisplacement, .amplitude = 4.0f}};
    const GpuSprite gb = makeGpuSprite(bare, 160, 144, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(g.row0[0], gb.row0[0]);
    EXPECT_FLOAT_EQ(g.row1[1], gb.row1[1]);
}

// ── The sprite record path ────────────────────────────────────────────────────────────────

TEST(BloomSpriteRecords, PacksTheResolvedKernelLanes) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom};
    e.radius = 6.0f; e.threshold = 51; e.intensity = 204;
    const SpriteFxRecord r = packSpriteFxRecord(e, /*isRegion=*/false, ShapePoints{}, 1.0f, BlendMode::Normal);
    const BloomParams    p = bloomParams(e);
    EXPECT_EQ(r.kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Bloom));
    EXPECT_FLOAT_EQ(r.params[0], p.radius);
    EXPECT_FLOAT_EQ(r.params[1], p.threshold);
    EXPECT_FLOAT_EQ(r.params[2], p.intensity);
    EXPECT_FLOAT_EQ(r.params[3], p.invNorm);
}

TEST(BloomSpriteRecords, EvalPassesBloomThroughUnchanged) {
    // The color-chain oracle leaves a Bloom step to the fragment's art sum — chain AND region records
    // pass the running color through (a region Bloom must never be painted as a fill color).
    Sprite s{.key = "b"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 4.0f, .intensity = 255}};
    s.regions = {Region{.key = "rg", .shape = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 4.0f,
                                                      .intensity = 255}}}};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_EQ(recs.size(), 2u);
    constexpr Vec4 base{0.4f, 0.5f, 0.6f, 1.0f};
    const auto out = evalSpriteFxRecords(base, 0.5f, 0.5f, 16, 16, recs);  // (8,8): inside the region
    ASSERT_TRUE(out.has_value());
    EXPECT_FLOAT_EQ(out->x, base.x);
    EXPECT_FLOAT_EQ(out->y, base.y);
    EXPECT_FLOAT_EQ(out->z, base.z);
    EXPECT_FLOAT_EQ(out->w, base.w);
}

}  // namespace
}  // namespace retropp
