#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"

// The built-in radial ripple effect. Device-free coverage of the CPU side: the uniform
// resolution (rippleParams — center px→UV normalization, the inverse-viewport scale) and the radial
// displacement math the ripple.frag GPU stage mirrors (rippleSourceUv). The live GPU path is build-
// compiled + dev-verified across all three backends (the documented CI-headless boundary); these are
// the failable units. amplitude is in viewport pixels and the sine/exp curve is GPU-verified, so the
// assertions pin center normalization + radial routing at SINE-EXACT arguments (sin(2π·0.25) = 1) on a
// SQUARE viewport (aspect = 1, so the corrected distance equals the plain UV distance).

namespace retropp {
namespace {

// rippleParams's center normalization is constexpr (pure arithmetic, no transcendentals). It cannot be
// asserted over a `constexpr ScreenSpaceEffect` instance — the struct carries a std::vector region, and
// a constexpr OBJECT with a heap-owning member is rejected by libstdc++ (the EngineConfig precedent). A
// constexpr wrapper FUNCTION builds the effect as a LOCAL temporary (constructed + destroyed within the
// constant evaluation, never persisted), which is well-formed, and exposes the resolved params for a
// genuine static_assert.
constexpr RippleParams rippleParamsOf(Point center, float amplitude, float frequency, float phase,
                                      float decay, PixelSize viewport) {
    ScreenSpaceEffect e{};
    e.kind      = ScreenSpaceEffectKind::Ripple;
    e.center    = center;
    e.amplitude = amplitude;
    e.frequency = frequency;
    e.phase     = phase;
    e.decay     = decay;
    return rippleParams(e, viewport);
}

// ── rippleParams — center px→UV normalization + invViewport ───────────────────────────

// center in viewport pixels normalizes to UV by the inverse viewport dimension: 80 px over 160 → 0.5,
// 72 px over 144 → 0.5. Genuinely constexpr → static_assert-testable (the displaceParams discipline).
TEST(RippleParams, CenterNormalizesPixelsToUv) {
    const RippleParams p = rippleParamsOf(Point{80.0f, 72.0f}, 0, 0, 0, 0, PixelSize{160, 144});
    EXPECT_FLOAT_EQ(p.centerU, 0.5f);
    EXPECT_FLOAT_EQ(p.centerV, 0.5f);
    static_assert(rippleParamsOf(Point{80.0f, 72.0f}, 0, 0, 0, 0, PixelSize{160, 144}).centerU == 0.5f,
                  "80 px over a 160 px viewport is centre-U 0.5");
    static_assert(rippleParamsOf(Point{80.0f, 72.0f}, 0, 0, 0, 0, PixelSize{160, 144}).centerV == 0.5f,
                  "72 px over a 144 px viewport is centre-V 0.5");
}

// The inverse-viewport scale (the px→UV amplitude scale, and the aspect via invH/invW).
TEST(RippleParams, InvViewportComputed) {
    const RippleParams p = rippleParamsOf(Point{0.0f, 0.0f}, 0, 0, 0, 0, PixelSize{160, 144});
    EXPECT_FLOAT_EQ(p.invViewportW, 1.0f / 160.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 1.0f / 144.0f);
}

// A degenerate (≤0) viewport dimension yields a 0 inverse — and thus a 0 normalized center on that axis
// — rather than dividing by zero. constexpr-checked.
TEST(RippleParams, DegenerateViewportZeroesInverseAndCenter) {
    const RippleParams p = rippleParamsOf(Point{80.0f, 72.0f}, 0, 0, 0, 0, PixelSize{0, 0});
    EXPECT_FLOAT_EQ(p.invViewportW, 0.0f);
    EXPECT_FLOAT_EQ(p.invViewportH, 0.0f);
    EXPECT_FLOAT_EQ(p.centerU, 0.0f);
    EXPECT_FLOAT_EQ(p.centerV, 0.0f);
    static_assert(rippleParamsOf(Point{80.0f, 72.0f}, 0, 0, 0, 0, PixelSize{0, 0}).centerU == 0.0f,
                  "a degenerate viewport zeroes the normalized centre, not a divide-by-zero");
}

// The scalar params (amplitude / frequency / phase / decay) are carried through verbatim.
TEST(RippleParams, CopiesScalarParams) {
    const RippleParams p = rippleParamsOf(Point{0.0f, 0.0f}, 6.0f, 7.0f, 0.25f, 2.5f, PixelSize{160, 144});
    EXPECT_FLOAT_EQ(p.amplitude, 6.0f);
    EXPECT_FLOAT_EQ(p.frequency, 7.0f);
    EXPECT_FLOAT_EQ(p.phase, 0.25f);
    EXPECT_FLOAT_EQ(p.decay, 2.5f);
}

// The RippleParams equality operator is constexpr (a plain struct, no vector — unlike its parent effect).
static_assert(RippleParams{} == RippleParams{}, "default RippleParams compare equal");
static_assert(!(RippleParams{.centerU = 0.5f} == RippleParams{}), "a differing field compares unequal");

// ── rippleSourceUv — the full radial mirror (identity, center, sign, decay) ────────────

// A square viewport so aspect = invH/invW = 1: the corrected distance equals the plain UV distance,
// keeping the sine-exact construction clean.
constexpr PixelSize kSquare{100, 100};

ScreenSpaceEffect makeRipple(Point center, float amplitude, float frequency, float phase, float decay) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Ripple, .amplitude = amplitude,
                             .frequency = frequency, .phase = phase, .center = center, .decay = decay};
}

// amplitude 0 → identity, regardless of frequency / phase / decay.
TEST(RippleSourceUv, IdentityAtZeroAmplitude) {
    const ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 0.0f, 6.0f, 0.25f, 2.0f);
    const Uv uv{0.4f, 0.6f};
    EXPECT_EQ(rippleSourceUv(uv, e, kSquare), uv);
}

// A non-Ripple kind is identity (the mirror is defensive — only kind == Ripple displaces).
TEST(RippleSourceUv, IdentityForNonRippleKind) {
    ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 9.0f, 6.0f, 0.0f, 0.0f);
    e.kind = ScreenSpaceEffectKind::RowDisplacement;  // amplitude is set, but the kind gates it out
    const Uv uv{0.3f, 0.7f};
    EXPECT_EQ(rippleSourceUv(uv, e, kSquare), uv);
}

// The center fragment (dist ≈ 0) has no radial direction → identity, even at non-zero amplitude.
TEST(RippleSourceUv, CenterFragmentIsIdentity) {
    const ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 10.0f, 1.0f, 0.0f, 0.0f);
    const Uv uv{0.5f, 0.5f};  // center in UV (50/100, 50/100)
    EXPECT_EQ(rippleSourceUv(uv, e, kSquare), uv);
}

// On the +x radius from the center: dx = 0.25, dist = 0.25 (aspect 1); at frequency 1 / phase 0 the
// sine argument is 2π·(1·0.25) → sin = 1, decay 0 → env = 1, so offset = amplitude px, displaced
// radially OUTWARD (dir = +x). 10 px over a 100 px viewport is +0.1 UV.
TEST(RippleSourceUv, SineExactRadialOffsetOutward) {
    const ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 10.0f, 1.0f, 0.0f, 0.0f);
    const Uv uv{0.75f, 0.5f};  // dx = +0.25
    const Uv src = rippleSourceUv(uv, e, kSquare);
    EXPECT_NEAR(src.u, 0.85f, 1e-4f);  // 0.75 + (+1)·(10·0.01)
    EXPECT_NEAR(src.v, 0.5f, 1e-4f);   // on-axis: dy = 0 → V unchanged
}

// On the −x side the radial direction reverses (dir = −x), so the SAME sine-1 crest pushes the sample
// further out toward 0 — the offset sign tracks the side, the ripple-vs-displace distinction.
TEST(RippleSourceUv, RadialDirectionFlipsSignAcrossCenter) {
    const ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 10.0f, 1.0f, 0.0f, 0.0f);
    const Uv uv{0.25f, 0.5f};  // dx = −0.25
    const Uv src = rippleSourceUv(uv, e, kSquare);
    EXPECT_NEAR(src.u, 0.15f, 1e-4f);  // 0.25 + (−1)·(10·0.01)
    EXPECT_NEAR(src.v, 0.5f, 1e-4f);
}

// decay > 0 fades the crest with radius: at two radii chosen so the sine is 1 at BOTH (frequency 4 →
// 4·r ∈ {0.25, 1.25}, both sin(2π·k) = 1), the inner ring's displacement magnitude exceeds the outer's
// (exp(−decay·r) is monotonically decreasing). This pins the radial falloff independent of the sine.
TEST(RippleSourceUv, DecayFalloffMonotonic) {
    const ScreenSpaceEffect e = makeRipple(Point{50.0f, 50.0f}, 10.0f, 4.0f, 0.0f, 2.0f);
    const Uv inner{0.5625f, 0.5f};  // r = 0.0625 → 4·r = 0.25
    const Uv outer{0.8125f, 0.5f};  // r = 0.3125 → 4·r = 1.25
    const float innerDisp = rippleSourceUv(inner, e, kSquare).u - inner.u;  // both positive (dir = +x, sin = 1)
    const float outerDisp = rippleSourceUv(outer, e, kSquare).u - outer.u;
    EXPECT_GT(innerDisp, 0.0f);
    EXPECT_GT(outerDisp, 0.0f);
    EXPECT_GT(innerDisp, outerDisp);  // exp(−2·0.0625) > exp(−2·0.3125)
}

}  // namespace
}  // namespace retropp
