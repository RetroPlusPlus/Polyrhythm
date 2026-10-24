// The Bloom / Glow emission chain's device-free CPU authorities: the chain plan (emissionChainPlan — the
// per-kind identity gates, the reduction threshold, and the kernel resolved at the blur's own resolution),
// the per-pixel emission (emissionExtractBloom / emissionExtractGlow — threshold, intensity and tint folded
// in ahead of any blur), and the additive finish (emissionComposite — equivalent to applyBloomAdd /
// applyGlowAdd with the intensity already carried by the emission).
//
// The load-bearing test here is SeparableEqualsTwoDimensionalGather: the chain replaced a single 2-D
// Gaussian gather with two 1-D sweeps, and that substitution is only legitimate because the kernel is
// exactly separable — w(dx)·w(dy), with the nonlinear per-pixel key applied at extract, before any
// summation. This composes both oracles over the same synthetic field and pins that they agree, which is
// what licenses the whole rewrite. The reduced path is deliberately NOT held to that standard (averaging
// the field first is a real, intended approximation); what it IS held to is a normalized kernel, so a halo
// carries the same energy whichever resolution produced it.

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

namespace retropp {
namespace {

// ── emissionChainPlan — gating, reduction, and the resolved kernel ─────────────────────────

TEST(EmissionChainPlan, ZeroIntensityIsAnIdentityForBothKinds) {
    ScreenSpaceEffect bloom{.kind = ScreenSpaceEffectKind::Bloom};
    bloom.radius = 12.0f;  // a wide reach earns nothing without strength
    EXPECT_FALSE(emissionChainPlan(bloom).engaged);

    ScreenSpaceEffect glow{.kind = ScreenSpaceEffectKind::Glow};
    glow.radius = 12.0f;
    EXPECT_FALSE(emissionChainPlan(glow).engaged);
}

// The gates differ by kind, and each reproduces what that kind has always done. Glow early-outs on a
// non-positive radius (no reach, no aura, whatever the intensity); Bloom does NOT — at radius 0 its kernel
// degenerates to a single center tap and it still adds its own un-blurred brightpass back over itself.
TEST(EmissionChainPlan, ZeroRadiusGatesGlowButNotBloom) {
    ScreenSpaceEffect glow{.kind = ScreenSpaceEffectKind::Glow};
    glow.radius = 0.0f; glow.intensity = 255;
    EXPECT_FALSE(emissionChainPlan(glow).engaged);
    glow.radius = -3.0f;  // negative reach resolves to none, not to a reflected disc
    EXPECT_FALSE(emissionChainPlan(glow).engaged);

    ScreenSpaceEffect bloom{.kind = ScreenSpaceEffectKind::Bloom};
    bloom.radius = 0.0f; bloom.intensity = 255;
    const EmissionChainPlan p = emissionChainPlan(bloom);
    EXPECT_TRUE(p.engaged);
    EXPECT_EQ(p.taps, 0);          // one center tap
    EXPECT_FALSE(p.downsample);
    EXPECT_FLOAT_EQ(p.invNorm, 1.0f);
}

TEST(EmissionChainPlan, ReductionEngagesAtTheThresholdRadiusAndScalesTheStep) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
    e.intensity = 255;

    e.radius = kEmissionDownsampleRadius - 0.5f;
    EmissionChainPlan p = emissionChainPlan(e);
    EXPECT_TRUE(p.engaged);
    EXPECT_FALSE(p.downsample);
    EXPECT_FLOAT_EQ(p.stepPx, 1.0f);  // taps one viewport pixel apart

    e.radius = kEmissionDownsampleRadius;  // the threshold itself reduces
    p = emissionChainPlan(e);
    EXPECT_TRUE(p.downsample);
    EXPECT_FLOAT_EQ(p.stepPx, static_cast<float>(kEmissionDownsampleFactor));
}

// The reduced path resolves its kernel at radius/factor, so the halo keeps the same size in the image: a
// tap covers four viewport pixels, and there are a quarter as many of them.
TEST(EmissionChainPlan, ResolvesTheKernelAtTheBlursOwnResolution) {
    ScreenSpaceEffect full{.kind = ScreenSpaceEffectKind::Bloom};
    full.radius = 5.0f; full.intensity = 255;
    const EmissionChainPlan pf = emissionChainPlan(full);
    const GaussianKernel    kf = gaussianKernel(5.0f);
    EXPECT_EQ(pf.taps, kf.taps);
    EXPECT_FLOAT_EQ(pf.invNorm, kf.invNorm);

    ScreenSpaceEffect wide{.kind = ScreenSpaceEffectKind::Bloom};
    wide.radius = 20.0f; wide.intensity = 255;
    const EmissionChainPlan pw = emissionChainPlan(wide);
    const GaussianKernel    kw = gaussianKernel(20.0f / static_cast<float>(kEmissionDownsampleFactor));
    EXPECT_TRUE(pw.downsample);
    EXPECT_EQ(pw.taps, kw.taps);
    EXPECT_EQ(pw.taps, 5);  // 41 taps per axis become 5 — the reduction's whole point
    EXPECT_FLOAT_EQ(pw.invNorm, kw.invNorm);
}

// σ is quartered exactly alongside the tap spacing, so the reduced Gaussian covers the same distance in
// viewport pixels as the full-resolution one it stands in for.
TEST(EmissionChainPlan, ReducedSigmaIsTheFullResolutionSigmaOverTheFactor) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Glow};
    e.radius = 20.0f; e.intensity = 255;
    const EmissionChainPlan p = emissionChainPlan(e);

    const float sigmaFull = std::max(20.0f, 0.5f) * 0.5f;
    const float sigmaLow  = sigmaFull / static_cast<float>(kEmissionDownsampleFactor);
    EXPECT_NEAR(p.inv2Sigma2, 1.0f / (2.0f * sigmaLow * sigmaLow), 1e-4f);
}

// Whichever path runs, the kernel each pass applies sums to 1 — so a halo neither gains nor loses energy
// when the reduction engages. This is the property that keeps the look continuous across the threshold.
TEST(EmissionChainPlan, TheNormalizedKernelSumsToOneOnBothPaths) {
    for (const float radius : {3.0f, 7.9f, 8.0f, 20.0f}) {
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom};
        e.radius = radius; e.intensity = 255;
        const EmissionChainPlan p = emissionChainPlan(e);
        const float blurRadius = radius / p.stepPx;

        float sum = 0.0f;
        for (int k = -p.taps; k <= p.taps; ++k) sum += gaussianKernelWeight(k, blurRadius);
        EXPECT_NEAR(sum * p.invNorm, 1.0f, 1e-5f) << "radius " << radius;
    }
}

// ── The separability proof ─────────────────────────────────────────────────────────────────

// A tiny scalar field with CLAMP_TO_EDGE reads, standing in for the emission target.
struct Field {
    int                width = 0, height = 0;
    std::vector<float> v;
    [[nodiscard]] float at(int x, int y) const {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return v[static_cast<std::size_t>(y * width + x)];
    }
};

// The gather the single-pass shaders ran: every (dx, dy) of the neighborhood, weighted by the product of
// the per-axis weights, normalized by invNorm².
[[nodiscard]] float twoDimensionalGather(const Field& f, int x, int y, float radius,
                                         const GaussianKernel& k) {
    float sum = 0.0f;
    for (int dy = -k.taps; dy <= k.taps; ++dy) {
        const float wy = gaussianKernelWeight(dy, radius);
        for (int dx = -k.taps; dx <= k.taps; ++dx) {
            sum += wy * gaussianKernelWeight(dx, radius) * f.at(x + dx, y + dy);
        }
    }
    return sum * k.invNorm * k.invNorm;
}

// One axis of the chain's blur, normalized once — the pair composes into the gather above.
[[nodiscard]] Field sweep(const Field& f, int stepX, int stepY, float radius, const GaussianKernel& k) {
    Field out{.width = f.width, .height = f.height,
              .v = std::vector<float>(f.v.size(), 0.0f)};
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            float sum = 0.0f;
            for (int i = -k.taps; i <= k.taps; ++i) {
                sum += gaussianKernelWeight(i, radius) * f.at(x + i * stepX, y + i * stepY);
            }
            out.v[static_cast<std::size_t>(y * out.width + x)] = sum * k.invNorm;
        }
    }
    return out;
}

// THE proof the rewrite rests on. Two 1-D sweeps reproduce the 2-D gather everywhere, because the weight
// is a product of its axes and the nonlinear key was already applied per-pixel at extract. If this fails,
// the chain is not a reimplementation of the effect — it is a different effect.
TEST(EmissionSeparability, SeparableEqualsTwoDimensionalGather) {
    constexpr float kRadius = 3.0f;
    const GaussianKernel k = gaussianKernel(kRadius);

    Field src{.width = 11, .height = 9, .v = std::vector<float>(11 * 9, 0.0f)};
    for (int y = 0; y < src.height; ++y) {          // an asymmetric field, so a transposed bug shows
        for (int x = 0; x < src.width; ++x) {
            src.v[static_cast<std::size_t>(y * src.width + x)] =
                0.1f * static_cast<float>(x) + 0.03f * static_cast<float>(y * y);
        }
    }
    src.v[static_cast<std::size_t>(4 * src.width + 5)] = 7.0f;  // a hot pixel, well inside

    const Field horizontal = sweep(src, 1, 0, kRadius, k);
    const Field separable  = sweep(horizontal, 0, 1, kRadius, k);

    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            const float gathered = twoDimensionalGather(src, x, y, kRadius, k);
            const float swept    = separable.v[static_cast<std::size_t>(y * separable.width + x)];
            EXPECT_NEAR(swept, gathered, 1e-4f) << "at (" << x << ", " << y << ")";
        }
    }
}

// ── The extract — threshold, intensity and tint fold in before any blur ────────────────────

TEST(EmissionExtract, BloomIsTheBrightpassScaledByIntensity) {
    const Vec4  src{0.8f, 0.6f, 0.4f, 1.0f};
    const float threshold = 0.25f, intensity = 0.5f;
    const Vec4  e = emissionExtractBloom(src, threshold, intensity);
    const Vec4  b = applyBrightpass(src, threshold);
    EXPECT_FLOAT_EQ(e.x, b.x * intensity);
    EXPECT_FLOAT_EQ(e.y, b.y * intensity);
    EXPECT_FLOAT_EQ(e.z, b.z * intensity);
    EXPECT_FLOAT_EQ(e.w, b.w * intensity);
}

TEST(EmissionExtract, GlowCarriesTheMaskOnAlphaAndTakesItsChromaFromTheTint) {
    // A deep-blue source under a warm tint: the emission must be warm, with no trace of the source hue.
    const Vec4  src{0.0f, 0.0f, 0.9f, 1.0f};
    const float intensity = 1.0f;
    const Vec4  e = emissionExtractGlow(src, /*threshold=*/0.0f, intensity, 1.0f, 0.4f, 0.1f);
    const float m = glowMask(src, 0.0f);
    EXPECT_FLOAT_EQ(e.w, m * intensity);
    EXPECT_FLOAT_EQ(e.x, 1.0f * m * intensity);
    EXPECT_FLOAT_EQ(e.y, 0.4f * m * intensity);
    EXPECT_FLOAT_EQ(e.z, 0.1f * m * intensity);
    EXPECT_GT(e.x, e.z);  // warm, though the source is pure blue
}

TEST(EmissionExtract, ZeroIntensityEmitsNothingForBothKinds) {
    const Vec4 src{0.9f, 0.9f, 0.9f, 1.0f};
    const Vec4 b = emissionExtractBloom(src, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(b.x, 0.0f);
    EXPECT_FLOAT_EQ(b.w, 0.0f);
    const Vec4 g = emissionExtractGlow(src, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(g.x, 0.0f);
    EXPECT_FLOAT_EQ(g.w, 0.0f);
}

// ── The composite — the same add the single-pass shaders performed ─────────────────────────

// The fold is exact: adding an emission that already carries the intensity equals the old form, which
// carried the intensity into the add. This is what lets one emission buffer hold many emitters at once.
TEST(EmissionCompositeMatch, EqualsApplyBloomAddWithTheIntensityFolded) {
    const Vec4  src{0.2f, 0.3f, 0.4f, 0.5f};
    const Vec4  glowField{0.6f, 0.5f, 0.4f, 0.8f};
    const float intensity = 0.25f;

    const Vec4 folded{glowField.x * intensity, glowField.y * intensity,
                      glowField.z * intensity, glowField.w * intensity};
    const Vec4 viaChain = emissionComposite(src, folded, /*glow=*/false);
    const Vec4 viaOld   = applyBloomAdd(src, glowField, intensity);
    EXPECT_NEAR(viaChain.x, viaOld.x, 1e-6f);
    EXPECT_NEAR(viaChain.y, viaOld.y, 1e-6f);
    EXPECT_NEAR(viaChain.z, viaOld.z, 1e-6f);
    EXPECT_NEAR(viaChain.w, viaOld.w, 1e-6f);
}

TEST(EmissionCompositeMatch, EqualsApplyGlowAddWithTheIntensityFolded) {
    const Vec4  src{0.2f, 0.3f, 0.4f, 0.5f};
    const float mask = 0.7f, intensity = 0.5f;
    const float tintR = 1.0f, tintG = 0.25f, tintB = 0.1f;

    const float lift = mask * intensity;
    const Vec4  folded{tintR * lift, tintG * lift, tintB * lift, lift};
    const Vec4  viaChain = emissionComposite(src, folded, /*glow=*/true);
    const Vec4  viaOld   = applyGlowAdd(src, mask, intensity, tintR, tintG, tintB);
    EXPECT_NEAR(viaChain.x, viaOld.x, 1e-6f);
    EXPECT_NEAR(viaChain.y, viaOld.y, 1e-6f);
    EXPECT_NEAR(viaChain.z, viaOld.z, 1e-6f);
    EXPECT_NEAR(viaChain.w, viaOld.w, 1e-6f);
}

TEST(EmissionCompositeMatch, AZeroEmissionLeavesTheSourceExactly) {
    const Vec4 src{0.2f, 0.3f, 0.4f, 0.5f};
    const Vec4 none{0.0f, 0.0f, 0.0f, 0.0f};
    for (const bool glow : {false, true}) {
        const Vec4 out = emissionComposite(src, none, glow);
        EXPECT_FLOAT_EQ(out.x, src.x);
        EXPECT_FLOAT_EQ(out.y, src.y);
        EXPECT_FLOAT_EQ(out.z, src.z);
        EXPECT_FLOAT_EQ(out.w, src.w);
    }
}

// The two kinds differ only in how the halo lifts coverage: Bloom's own light fills what the source does
// not already cover, so an opaque source stays put; Glow's authored aura lifts directly and saturates.
TEST(EmissionCompositeMatch, TheAlphaRuleIsTheOnlyDifferenceBetweenTheKinds) {
    const Vec4 opaque{0.1f, 0.1f, 0.1f, 1.0f};
    const Vec4 e{0.0f, 0.0f, 0.0f, 0.5f};
    EXPECT_FLOAT_EQ(emissionComposite(opaque, e, /*glow=*/false).w, 1.0f);  // (1 − a) = 0
    EXPECT_FLOAT_EQ(emissionComposite(opaque, e, /*glow=*/true).w, 1.0f);   // saturated

    const Vec4 half{0.1f, 0.1f, 0.1f, 0.5f};
    EXPECT_FLOAT_EQ(emissionComposite(half, e, /*glow=*/false).w, 0.75f);   // 0.5 + 0.5·0.5
    EXPECT_FLOAT_EQ(emissionComposite(half, e, /*glow=*/true).w, 1.0f);     // 0.5 + 0.5
}

// The rgb sum is deliberately unclamped — the float16 chain carries a hot halo above 1 to the blit.
TEST(EmissionCompositeMatch, TheRgbSumIsNotClamped) {
    const Vec4 src{0.9f, 0.9f, 0.9f, 1.0f};
    const Vec4 hot{0.8f, 0.8f, 0.8f, 0.0f};
    EXPECT_GT(emissionComposite(src, hot, /*glow=*/true).x, 1.0f);
}

}  // namespace
}  // namespace retropp
