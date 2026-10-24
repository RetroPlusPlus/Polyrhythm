#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"

// The container blend modes. Device-free coverage of the CPU side — applyBlendMode, the single authority
// the region-select gate and the blend composite shader mirror: the separable operator B(dst, src) per
// mode, applied source-alpha-weighted, with the standard over alpha. Normal reduces to plain alpha-over
// (the byte-identity anchor — a Normal container is unchanged). The math is pure arithmetic — genuinely
// constexpr — so it is static_assert-testable; the live GPU path is build-compiled + dev-verified across
// the backends (the documented CI-headless boundary).

namespace retropp {
namespace {

// A blend at full source coverage (src.a = 1): out.rgb is exactly the operator B(dst, src), out.a = 1.
constexpr Vec4 opaqueBlend(Vec4 dst, Vec4 src, BlendMode mode) {
    return applyBlendMode(dst, Vec4{src.x, src.y, src.z, 1.0f}, mode);
}

// ── blendChannel — the per-channel separable operator ─────────────────────────────────

TEST(BlendChannel, EachModeOperator) {
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Normal, 0.5f, 0.25f), 0.25f);   // src
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Add, 0.5f, 0.25f), 0.75f);      // dst + src
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Subtract, 0.5f, 0.25f), 0.25f); // dst - src
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Multiply, 0.5f, 0.5f), 0.25f);  // dst * src
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Screen, 0.5f, 0.5f), 0.75f);    // 1 - (1-dst)(1-src)
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Half, 0.5f, 0.25f), 0.375f);    // (dst + src) / 2
    // blendChannel does not clamp — the raw operator can leave the range; applyBlendMode clamps.
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Add, 0.8f, 0.5f), 1.3f);
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Subtract, 0.2f, 0.5f), -0.3f);
    static_assert(blendChannel(BlendMode::Screen, 0.0f, 0.0f) == 0.0f, "Screen of black is black");
    static_assert(blendChannel(BlendMode::Multiply, 1.0f, 0.3f) == 0.3f, "Multiply by white is identity");
}

// ── applyBlendMode — Normal reduces to alpha-over ─────────────────────────────────────

// Normal is exactly the alpha-over the compositor always ran — at every source alpha, on every channel
// (including alpha). This is the byte-identity guarantee: a default (Normal) container is unchanged.
TEST(ApplyBlendMode, NormalIsAlphaOver) {
    constexpr Vec4 dst{0.2f, 0.4f, 0.6f, 0.7f};
    for (float sa : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const Vec4 src{0.8f, 0.1f, 0.3f, sa};
        const Vec4 got = applyBlendMode(dst, src, BlendMode::Normal);
        const Vec4 ref = alphaOver(dst, src);
        EXPECT_FLOAT_EQ(got.x, ref.x);
        EXPECT_FLOAT_EQ(got.y, ref.y);
        EXPECT_FLOAT_EQ(got.z, ref.z);
        EXPECT_FLOAT_EQ(got.w, ref.w);
    }
    // A fully transparent source leaves the destination untouched (over identity).
    static_assert(applyBlendMode(Vec4{0.2f, 0.4f, 0.6f, 1.0f}, Vec4{0.9f, 0.9f, 0.9f, 0.0f},
                                 BlendMode::Normal) == Vec4{0.2f, 0.4f, 0.6f, 1.0f},
                  "a transparent source is a no-op over the destination");
}

// ── applyBlendMode — each operator over an opaque backdrop ─────────────────────────────

// With an opaque source over an opaque backdrop the source-alpha weighting drops out and out.rgb is the
// pure operator B(dst, src). Anchors each mode's math at binary-exact values.
TEST(ApplyBlendMode, OpaqueOperators) {
    constexpr Vec4 d{0.5f, 0.5f, 0.5f, 1.0f};
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.25f, 0, 0, 1}, BlendMode::Add).x, 0.75f);
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.25f, 0, 0, 1}, BlendMode::Subtract).x, 0.25f);
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.5f, 0, 0, 1}, BlendMode::Multiply).x, 0.25f);
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.5f, 0, 0, 1}, BlendMode::Screen).x, 0.75f);
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.25f, 0, 0, 1}, BlendMode::Half).x, 0.375f);
    // out.a is 1 for an opaque source over an opaque backdrop, every mode.
    EXPECT_FLOAT_EQ(opaqueBlend(d, Vec4{0.25f, 0, 0, 1}, BlendMode::Add).w, 1.0f);
}

// ── applyBlendMode — clamping at the range extremes ────────────────────────────────────

// Add saturates at 1; Subtract floors at 0. The clamp keeps the output a valid color.
TEST(ApplyBlendMode, ClampsToUnitRange) {
    EXPECT_FLOAT_EQ(opaqueBlend(Vec4{0.8f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Add).x, 1.0f);
    EXPECT_FLOAT_EQ(opaqueBlend(Vec4{0.2f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Subtract).x, 0.0f);
    static_assert(applyBlendMode(Vec4{1.0f, 0, 0, 1}, Vec4{1.0f, 0, 0, 1}, BlendMode::Add).x == 1.0f,
                  "Add clamps to 1");
    static_assert(applyBlendMode(Vec4{0.0f, 0, 0, 1}, Vec4{1.0f, 0, 0, 1}, BlendMode::Subtract).x == 0.0f,
                  "Subtract clamps to 0");
}

// ── applyBlendMode — source-alpha weighting ────────────────────────────────────────────

// A half-transparent source contributes half of the blended operator and half of the backdrop. At src.a
// = 0.5 over an opaque multiply: out = 0.5·dst + 0.5·(dst·src).
TEST(ApplyBlendMode, SourceAlphaWeightsTheOperator) {
    constexpr Vec4 dst{0.8f, 0.8f, 0.8f, 1.0f};
    constexpr Vec4 src{0.5f, 0.5f, 0.5f, 0.5f};  // half-strength multiply tint
    const Vec4 got = applyBlendMode(dst, src, BlendMode::Multiply);
    // 0.5·0.8 + 0.5·(0.8·0.5) = 0.4 + 0.2 = 0.6
    EXPECT_FLOAT_EQ(got.x, 0.6f);
    EXPECT_FLOAT_EQ(got.w, 1.0f);  // over an opaque backdrop the result stays opaque
}

// ── ColorFill grade equivalence (Region::blend) ────────────────────────────────────────

// A ColorFill effect is a solid color SOURCE (out.rgb = fill, opaque). Its owning Region's blend grades
// how that color combines over the scene: blend = Multiply darkens the scene by the fill (a shadow /
// tint), blend = Add brightens it (a glow). The grade is exactly applyBlendMode — the dropped ColorFill
// mul/add grade lands here.
TEST(ColorFillGrade, MultiplyTintAndAddGlow) {
    constexpr Vec4 scene{0.6f, 0.6f, 0.6f, 1.0f};
    constexpr Vec4 fill{0.5f, 0.5f, 0.5f, 1.0f};  // the ColorFill color as an opaque source
    // Multiply grade = scene · fill (a 50% shadow).
    EXPECT_FLOAT_EQ(applyBlendMode(scene, fill, BlendMode::Multiply).x, 0.3f);
    // Add grade = scene + fill, clamped (a glow toward white).
    EXPECT_FLOAT_EQ(applyBlendMode(scene, fill, BlendMode::Add).x, 1.0f);
    static_assert(applyBlendMode(Vec4{0.6f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Multiply).x == 0.3f,
                  "Multiply ColorFill grade darkens the scene by the fill");
}

// Vec4 equality is constexpr (geometry.h), so the operator anchors are compile-time.
static_assert(applyBlendMode(Vec4{}, Vec4{}, BlendMode::Normal) == Vec4{}, "blend of zeros is zero");

}  // namespace
}  // namespace retropp
