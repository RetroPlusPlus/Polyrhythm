// The Glow effect's device-free CPU authorities: the resolved parameters (glowParams — kernel via the shared
// gaussianKernel, uint8 knobs normalized, the authored tint composed from fill × fillIntensity), the scalar
// emission mask (glowMask — coverage × straight-luminance survival, whole-coverage emission at threshold 0),
// the tinted additive composite (applyGlowAdd — the halo's chroma is the tint, never the source hue), the
// sprite reach + flags (spriteRadialReach / makeGpuSprite), the sprite record path (packSpriteFxRecord kernel
// lanes + the tint on the gate lanes; region Glow steps are unsupported and skipped), and the compose-skip
// fingerprint (a tint change under a settled Glow frame reprints). A composed mini-oracle sums a synthetic
// art the way the fragment's kernel loop does and pins the aura's shape and chroma.

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"

namespace retropp {
namespace {

// ── glowParams — the resolved kernel + knobs + tint ───────────────────────────────────────

TEST(GlowParamsResolve, NormalizesKnobsComposesTintAndSharesTheKernel) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
    e.fill = Rgba8{.r = 255, .g = 66, .b = 26, .a = 255};  // ember
    e.radius = 4.0f; e.threshold = 128; e.intensity = 255;
    const GlowParams p = glowParams(e);
    EXPECT_FLOAT_EQ(p.radius, 4.0f);
    EXPECT_EQ(p.taps, 4);
    EXPECT_NEAR(p.threshold, 128.0f / 255.0f, 1e-6f);
    EXPECT_FLOAT_EQ(p.intensity, 1.0f);
    EXPECT_FLOAT_EQ(p.tintR, 1.0f);
    EXPECT_NEAR(p.tintG, 66.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(p.tintB, 26.0f / 255.0f, 1e-6f);
    // The kernel is the SHARED resolver — identical taps + normalization to a Bloom of the same radius.
    const GaussianKernel k = gaussianKernel(4.0f);
    EXPECT_EQ(p.taps, k.taps);
    EXPECT_FLOAT_EQ(p.invNorm, k.invNorm);
    ScreenSpaceEffect b{.kind = ScreenSpaceEffectKind::Bloom};
    b.radius = 4.0f;
    const BloomParams bp = bloomParams(b);
    EXPECT_EQ(p.taps, bp.taps);
    EXPECT_FLOAT_EQ(p.invNorm, bp.invNorm);
}

TEST(GlowParamsResolve, FillIntensityScalesTheTintAndDefaultsAreIdentity) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
    e.fill = Rgba8{.r = 128, .g = 64, .b = 32, .a = 255};
    e.fillIntensity = 2.0f;  // an HDR-hot aura — the tint exceeds 1 on the float16 chain
    e.radius = 3.0f;
    const GlowParams p = glowParams(e);
    EXPECT_NEAR(p.tintR, 2.0f * 128.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(p.tintG, 2.0f * 64.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(p.tintB, 2.0f * 32.0f / 255.0f, 1e-6f);

    ScreenSpaceEffect neg{.kind = ScreenSpaceEffectKind::Glow};
    neg.radius = -5.0f;                     // a negative radius is no reach
    EXPECT_FLOAT_EQ(glowParams(neg).radius, 0.0f);
    EXPECT_EQ(glowParams(neg).taps, 0);

    const GlowParams d = glowParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow});
    EXPECT_FLOAT_EQ(d.radius, 0.0f);        // the identity default: no reach…
    EXPECT_FLOAT_EQ(d.intensity, 0.0f);     // …and no strength
}

// ── glowMask — the scalar emission mask ───────────────────────────────────────────────────

TEST(GlowMaskEmission, ThresholdZeroIsWholeCoverageEmission) {
    // Every covered pixel emits its coverage — dark content included (the aura a dark shape radiates).
    constexpr Vec4 darkOpaque{0.0f, 0.0f, 0.0f, 1.0f};
    static_assert(glowMask(darkOpaque, 0.0f) == 1.0f, "dark opaque content emits fully at threshold 0");
    constexpr Vec4 halfCover{0.1f, 0.1f, 0.1f, 0.5f};
    static_assert(glowMask(halfCover, 0.0f) == 0.5f, "the mask rides the coverage");
    static_assert(glowMask(Vec4{0.0f, 0.0f, 0.0f, 0.0f}, 0.0f) == 0.0f, "no coverage, no emission");
}

TEST(GlowMaskEmission, ThresholdKeysOnTheStraightLuminance) {
    // A premultiplied dim pixel whose STRAIGHT color is white: the key must read the straight luminance
    // (1.0), not the coverage-dimmed one (0.25) — un-premultiply before keying.
    const float m = glowMask(Vec4{0.25f, 0.25f, 0.25f, 0.25f}, 0.5f);
    EXPECT_NEAR(m, 0.25f, 1e-6f);  // survives fully (straight lum 1 ≥ 0.5), mask = coverage
    // Dark content below the floor emits nothing.
    EXPECT_FLOAT_EQ(glowMask(Vec4{0.1f, 0.1f, 0.1f, 1.0f}, 0.5f), 0.0f);
    // The rescale law: a survivor rescales toward full strength — straight lum 0.75 at threshold 0.5.
    EXPECT_NEAR(glowMask(Vec4{0.75f, 0.75f, 0.75f, 1.0f}, 0.5f), 0.5f, 1e-5f);  // (0.75−0.5)/0.5
}

// ── applyGlowAdd — the tinted additive composite ──────────────────────────────────────────

TEST(GlowAdd, IdentityAtZeroIntensityOrZeroMask) {
    constexpr Vec4 src{0.3f, 0.5f, 0.7f, 0.6f};
    static_assert(applyGlowAdd(src, 0.9f, 0.0f, 1.0f, 0.3f, 0.1f) == src,
                  "intensity 0 adds nothing — byte-exact identity");
    static_assert(applyGlowAdd(src, 0.0f, 1.0f, 1.0f, 0.3f, 0.1f) == src,
                  "a zero mask adds nothing — byte-exact identity");
}

TEST(GlowAdd, TheHaloChromaIsTheTintNeverTheSource) {
    // A BLUE source gains a pure-ember delta: out − src is exactly lift·tint per channel — no source hue
    // enters the added term (the definitional line against Bloom).
    constexpr Vec4  blue{0.0f, 0.0f, 0.8f, 1.0f};
    constexpr float tintR = 1.0f, tintG = 0.26f, tintB = 0.10f;  // ember
    const Vec4 out  = applyGlowAdd(blue, 0.5f, 1.0f, tintR, tintG, tintB);
    const float lift = 1.0f * 0.5f;
    EXPECT_NEAR(out.x - blue.x, lift * tintR, 1e-6f);
    EXPECT_NEAR(out.y - blue.y, lift * tintG, 1e-6f);
    EXPECT_NEAR(out.z - blue.z, lift * tintB, 1e-6f);
    EXPECT_GT(out.x - blue.x, out.z - blue.z);  // the delta is r-dominant like the tint, not b like the source
}

TEST(GlowAdd, LiftsCoverageAndSaturatesAlpha) {
    const Vec4 out = applyGlowAdd(Vec4{0.2f, 0.2f, 0.2f, 0.5f}, 0.6f, 1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(out.w, 1.0f, 1e-6f);            // 0.5 + 0.6 saturates
    const Vec4 partial = applyGlowAdd(Vec4{0.2f, 0.2f, 0.2f, 0.3f}, 0.4f, 0.5f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(partial.w, 0.5f, 1e-6f);        // 0.3 + 0.5·0.4
    // The rgb sum is unclamped (float16 headroom to the blit).
    const Vec4 hot = applyGlowAdd(Vec4{0.9f, 0.9f, 0.9f, 1.0f}, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f);
    EXPECT_NEAR(hot.x, 2.9f, 1e-6f);
}

// ── The composed aura oracle — the fragment's kernel loop over a synthetic art ────────────

// One dark-but-opaque texel in a transparent 5×5 field, threshold 0: the aura at a neighbor decays with
// distance, is zero beyond the tap extent, and its chroma is the tint — composed from the same pure pieces
// the fragment loops over. Dark art radiating an authored color is the effect's signature.
TEST(GlowOracle, DarkTexelRadiatesTheTintAndFadesOut) {
    constexpr int   kSide   = 5;
    constexpr float kRadius = 2.0f;
    const GlowParams p = [] {
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
        e.fill = Rgba8{.r = 255, .g = 66, .b = 26, .a = 255};
        e.radius = kRadius; e.intensity = 255;
        return glowParams(e);
    }();
    std::array<Vec4, kSide * kSide> art{};                   // transparent everywhere…
    art[2 * kSide + 2] = Vec4{0.0f, 0.0f, 0.05f, 1.0f};      // …except a near-black center (premultiplied)

    auto maskAt = [&](int px, int py) {
        float m = 0.0f;
        for (int dy = -p.taps; dy <= p.taps; ++dy)
            for (int dx = -p.taps; dx <= p.taps; ++dx) {
                const int ax = px + dx, ay = py + dy;
                if (ax < 0 || ax >= kSide || ay < 0 || ay >= kSide) continue;  // off-art: transparent
                const float w = gaussianKernelWeight(dx, p.radius) * gaussianKernelWeight(dy, p.radius);
                m += w * glowMask(art[static_cast<std::size_t>(ay) * kSide + ax], p.threshold);
            }
        return m * p.invNorm * p.invNorm;
    };

    const float atCentre = maskAt(2, 2);
    const float oneOff   = maskAt(3, 2);
    const float twoOff   = maskAt(4, 2);
    EXPECT_GT(atCentre, oneOff);        // the aura decays outward
    EXPECT_GT(oneOff, twoOff);
    EXPECT_GT(twoOff, 0.0f);            // still lit at the tap extent — the DARK texel emits (threshold 0)
    // The aura's color at a neighbor is the tint: the composite's delta is lift·tint, r-dominant, though
    // the emitting art is blue.
    constexpr Vec4 bg{0.0f, 0.0f, 0.0f, 0.0f};
    const Vec4 lit = applyGlowAdd(bg, oneOff, p.intensity, p.tintR, p.tintG, p.tintB);
    EXPECT_GT(lit.x, lit.z);            // ember: r > b
    EXPECT_NEAR(lit.y / lit.x, p.tintG / p.tintR, 1e-4f);  // the exact tint ratio, no source term
}

// ── Sprite emission steps ─────────────────────────────────────────────────────────────────

TEST(GlowSpriteEmission, LayerScopeStepEmitsBelowScopeDoesNot) {
    Sprite s{.key = "g"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 5.0f, .intensity = 255}};
    const auto steps = spriteEmissionSteps(s, 1.0f);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].recordIndex, 0u);
    EXPECT_TRUE(steps[0].glow);
    EXPECT_FLOAT_EQ(steps[0].reach, 5.0f);

    Sprite below{.key = "g2"};
    below.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow,
                                       .scope = ScreenSpaceEffectScope::Below, .radius = 5.0f,
                                       .intensity = 255}};
    EXPECT_TRUE(spriteEmissionSteps(below, 1.0f).empty());  // a scene lens is the below pass, not a raster

    // Each realized step is its own emission — Bloom and Glow do not collapse into one reach.
    Sprite mixed{.key = "g3"};
    mixed.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 3.0f, .intensity = 255},
                     ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 7.0f, .intensity = 255}};
    const auto both = spriteEmissionSteps(mixed, 1.0f);
    ASSERT_EQ(both.size(), 2u);
    EXPECT_EQ(both[0].recordIndex, 0u);
    EXPECT_FALSE(both[0].glow);
    EXPECT_EQ(both[1].recordIndex, 1u);
    EXPECT_TRUE(both[1].glow);
}

TEST(GlowSpriteEmission, IdentityStepsCostNothingAndIndicesSkipNonGlowSteps) {
    // A Glow with no intensity, and one with no reach, are identities — neither becomes a raster.
    Sprite dark{.key = "d"};
    dark.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 5.0f, .intensity = 0},
                    ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 0.0f, .intensity = 255}};
    EXPECT_TRUE(spriteEmissionSteps(dark, 1.0f).empty());

    // recordIndex addresses the packed record slice, so the color steps before the glow count.
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::None},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation},
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f, .intensity = 255}};
    const auto steps = spriteEmissionSteps(s, 1.0f);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].recordIndex, 2u);                    // None is not packed; the two color steps are
    ASSERT_EQ(buildSpriteFxRecords(s).size(), 3u);
    EXPECT_EQ(buildSpriteFxRecords(s)[steps[0].recordIndex].kind,
              static_cast<std::uint32_t>(ScreenSpaceEffectKind::Glow));
}

TEST(GlowSpriteEmission, ReachIsAuthoredInArtPixelsAndBlursInViewportPixels) {
    // A sprite scaled 3× covers three viewport pixels per art pixel, so its halo must widen with it.
    Sprite s{.key = "g"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f, .intensity = 255}};
    EXPECT_FLOAT_EQ(spriteEmissionSteps(s, 3.0f)[0].reach, 12.0f);

    EXPECT_FLOAT_EQ(spriteLinearScale(s, Transform{}), 1.0f);                 // the identity pair
    s.transform = Transform::scale(3.0f, 3.0f);
    EXPECT_FLOAT_EQ(spriteLinearScale(s, Transform{}), 3.0f);
    s.transform = Transform::scale(4.0f, 1.0f);                              // non-uniform → one isotropic factor
    EXPECT_FLOAT_EQ(spriteLinearScale(s, Transform{}), 2.0f);
    s.transform = Transform{};
    EXPECT_FLOAT_EQ(spriteLinearScale(s, Transform::scale(2.0f, 2.0f)), 2.0f);  // the layer scales it too
}

TEST(GlowSpriteEmission, AnAuraDoesNotInflateTheArtFootprint) {
    Sprite s{.key = "aura", .x = 10, .y = 10};
    s.size = AssetDimensions{16, 16};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f, .intensity = 255}};
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    // The halo rasters into the emission buffer and spreads there, so the art keeps its own footprint —
    // the cheap plain path, byte-identical to the same sprite carrying no glow.
    Sprite plain{.key = "plain", .x = 10, .y = 10};
    plain.size = AssetDimensions{16, 16};
    const GpuSprite gp = makeGpuSprite(plain, 160, 144, 0.0f, 0.0f);
    EXPECT_EQ(g.flags, gp.flags);
    EXPECT_EQ(g.flags & kSpriteAnalyticFlag, 0u);
    EXPECT_EQ(g.flags & kSpriteHasDisplacementFlag, 0u);   // a glow is not a displacement pre-pass
    EXPECT_FLOAT_EQ(g.row0[0], gp.row0[0]);                // and no inflation entered the forward map
    EXPECT_FLOAT_EQ(g.row0[2], gp.row0[2]);
}

TEST(GlowSpriteEmission, BucketsGroupByKindAndReachNotByOrder) {
    // Two sprites at one reach share a bucket however they interleave with a third at another; a Bloom never
    // joins a Glow's bucket (their halos composite under different alpha rules).
    const std::vector<SpriteEmissionStep> steps{
        SpriteEmissionStep{0u, true,  6.0f},
        SpriteEmissionStep{0u, false, 6.0f},
        SpriteEmissionStep{1u, true,  6.0f},
        SpriteEmissionStep{0u, true, 20.0f},
    };
    const auto buckets = groupSpriteEmissionBuckets(steps);
    ASSERT_EQ(buckets.size(), 3u);
    EXPECT_TRUE(buckets[0].glow);
    EXPECT_FLOAT_EQ(buckets[0].reach, 6.0f);
    EXPECT_EQ(buckets[0].members, (std::vector<std::size_t>{0u, 2u}));   // non-contiguous, still one pass
    EXPECT_FALSE(buckets[1].glow);
    EXPECT_EQ(buckets[1].members, (std::vector<std::size_t>{1u}));
    EXPECT_FLOAT_EQ(buckets[2].reach, 20.0f);

    EXPECT_TRUE(groupSpriteEmissionBuckets({}).empty());
}

// ── The sprite record path ────────────────────────────────────────────────────────────────

TEST(GlowSpriteRecords, PacksTheKernelLanesAndTheTintOnTheGateLanes) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
    e.fill = Rgba8{.r = 255, .g = 66, .b = 26, .a = 255};
    e.radius = 6.0f; e.threshold = 51; e.intensity = 204;
    const SpriteFxRecord r = packSpriteFxRecord(e, /*isRegion=*/false, ShapePoints{}, 1.0f, BlendMode::Normal);
    const GlowParams     p = glowParams(e);
    EXPECT_EQ(r.kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Glow));
    EXPECT_FLOAT_EQ(r.params[0], p.radius);
    EXPECT_FLOAT_EQ(r.params[1], p.threshold);
    EXPECT_FLOAT_EQ(r.params[2], p.intensity);
    EXPECT_FLOAT_EQ(r.params[3], p.invNorm);
    EXPECT_FLOAT_EQ(r.radius, p.tintR);        // the tint rides the idle chain-step gate lanes
    EXPECT_FLOAT_EQ(r.strokeWidth, p.tintG);
    EXPECT_FLOAT_EQ(r.pad0, p.tintB);
}

TEST(GlowSpriteRecords, RegionGlowIsSkippedTheChainStepIsKept) {
    // A Glow's tint occupies the record lanes a region's shape needs, so a region-confined Glow step has no
    // packing — the builders drop it (the renderer warns) while the whole-silhouette chain step packs.
    Sprite s{.key = "g"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f, .intensity = 255}};
    s.regions = {Region{.key = "rg", .shape = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f,
                                                      .intensity = 255}}}};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_EQ(recs.size(), 1u);  // the chain step only
    EXPECT_EQ(recs[0].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Glow));
    EXPECT_EQ(recs[0].flags & kSpriteFxIsRegion, 0u);

    // Same rule on the scene-facing path: a Below chain Glow packs, a Below region Glow is dropped.
    Sprite lens{.key = "g2"};
    lens.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow,
                                      .scope = ScreenSpaceEffectScope::Below, .radius = 4.0f,
                                      .intensity = 255}};
    lens.regions = {Region{.key = "rg", .shape = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                           .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow,
                                                         .scope = ScreenSpaceEffectScope::Below,
                                                         .radius = 4.0f, .intensity = 255}}}};
    const std::vector<SpriteFxRecord> belowRecs = buildSpriteBelowRecords(lens);
    ASSERT_EQ(belowRecs.size(), 1u);
    EXPECT_EQ(belowRecs[0].flags & kSpriteFxIsRegion, 0u);
    EXPECT_TRUE(belowSpriteKindSupported(ScreenSpaceEffectKind::Glow));
}

TEST(GlowSpriteRecords, EvalPassesGlowThroughUnchanged) {
    // The color-chain oracle leaves a Glow step to the fragment's art sum — the record passes the running
    // color through (its params are kernel knobs + tint, never a fill).
    Sprite s{.key = "g"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .radius = 4.0f, .intensity = 255}};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_EQ(recs.size(), 1u);
    constexpr Vec4 base{0.4f, 0.5f, 0.6f, 1.0f};
    const auto out = evalSpriteFxRecords(base, 0.5f, 0.5f, 16, 16, recs);
    ASSERT_TRUE(out.has_value());
    EXPECT_FLOAT_EQ(out->x, base.x);
    EXPECT_FLOAT_EQ(out->y, base.y);
    EXPECT_FLOAT_EQ(out->z, base.z);
    EXPECT_FLOAT_EQ(out->w, base.w);
}

// ── The compose-skip fingerprint ──────────────────────────────────────────────────────────

TEST(GlowFingerprint, EveryConsultedFieldReprints) {
    // A settled frame skips recompose on an equal fingerprint — so every field Glow consults must fold.
    auto frameWith = [](ScreenSpaceEffect e) {
        FrameDrawState f;
        f.postEffects.push_back(e);
        return f;
    };
    ScreenSpaceEffect base{.kind = ScreenSpaceEffectKind::Glow};
    base.fill = Rgba8{.r = 255, .g = 66, .b = 26, .a = 255};
    base.radius = 6.0f; base.intensity = 200;
    const std::uint64_t h0 = hashFrameStructure(frameWith(base));
    EXPECT_EQ(h0, hashFrameStructure(frameWith(base)));  // deterministic

    ScreenSpaceEffect tinted = base;
    tinted.fill = Rgba8{.r = 64, .g = 66, .b = 255, .a = 255};
    EXPECT_NE(h0, hashFrameStructure(frameWith(tinted)));  // a tint change recomposes

    ScreenSpaceEffect hotter = base;
    hotter.fillIntensity = 2.0f;
    EXPECT_NE(h0, hashFrameStructure(frameWith(hotter)));

    ScreenSpaceEffect wider = base;
    wider.radius = 9.0f;
    EXPECT_NE(h0, hashFrameStructure(frameWith(wider)));

    ScreenSpaceEffect keyed = base;
    keyed.threshold = 128;
    EXPECT_NE(h0, hashFrameStructure(frameWith(keyed)));

    ScreenSpaceEffect stronger = base;
    stronger.intensity = 255;
    EXPECT_NE(h0, hashFrameStructure(frameWith(stronger)));
}

}  // namespace
}  // namespace retropp
