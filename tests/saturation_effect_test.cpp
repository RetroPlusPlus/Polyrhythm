#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"

// The built-in color-saturation effect. Device-free coverage of the CPU side: the uniform resolution
// (saturationParams — the developer's uint8 field normalized to [0,1]) and the color transform the
// saturation.frag GPU stage mirrors (applySaturation: each channel pulled toward the pixel's own luminance,
// so saturation == 1 is an exact identity and saturation == 0 is grayscale). The transform is pure arithmetic
// — genuinely constexpr — so it is static_assert-testable. The live GPU path is build-compiled + dev-verified
// across the backends (the documented CI-headless boundary).

namespace retropp {
namespace {

// saturationParams is constexpr but cannot be asserted over a `constexpr ScreenSpaceEffect` (its heap-owning
// ObjectKey member makes such an object ill-formed); a constexpr wrapper builds the effect as a LOCAL
// temporary and returns the resolved params.
constexpr SaturationParams saturationParamsOf(std::uint8_t saturation) {
    ScreenSpaceEffect e{};
    e.kind       = ScreenSpaceEffectKind::ColorSaturation;
    e.saturation = saturation;
    return saturationParams(e);
}

// ── saturationParams — the uint8 field normalized 0..255 -> 0..1 ────────────────────────

TEST(SaturationParams, NormalizesTheUint8Field) {
    EXPECT_FLOAT_EQ(saturationParamsOf(255).saturation, 1.0f);          // full saturation = identity
    EXPECT_FLOAT_EQ(saturationParamsOf(0).saturation, 0.0f);            // grayscale
    EXPECT_FLOAT_EQ(saturationParamsOf(128).saturation, 128.0f / 255.0f);
    static_assert(saturationParamsOf(255) == SaturationParams{1.0f},
                  "255 normalizes to full saturation (identity)");
    static_assert(saturationParamsOf(0) == SaturationParams{0.0f}, "0 normalizes to greyscale");
}

// The default field is 255 -> identity, so an unset ColorSaturation is a no-op.
TEST(SaturationParams, DefaultFieldIsIdentity) {
    ScreenSpaceEffect e{};
    e.kind = ScreenSpaceEffectKind::ColorSaturation;
    EXPECT_EQ(e.saturation, 255);
    EXPECT_FLOAT_EQ(saturationParams(e).saturation, 1.0f);
}

// SaturationParams equality is constexpr (a plain struct, no heap).
static_assert(SaturationParams{} == SaturationParams{}, "default SaturationParams compare equal");
static_assert(!(SaturationParams{0.5f} == SaturationParams{}), "a differing value compares unequal");

// ── applySaturation — the desaturation transform mirror ─────────────────────────────────

// saturation == 1 is an EXACT identity: every channel is returned unchanged, for any color. This is the
// crux — the default ColorSaturation (255 -> 1.0) is a no-op, so an inactive saturation never perturbs the
// scene, and the "identity params -> unchanged" golden holds. Exactness comes from the amount == 0 form
// (subtract-zero), not a lerp toward gray.
TEST(ApplySaturation, FullSaturationIsExactIdentity) {
    const ColorFillRgb c{0.8f, 0.2f, 0.5f};
    const ColorFillRgb out = applySaturation(c, SaturationParams{1.0f});
    EXPECT_FLOAT_EQ(out.r, c.r);
    EXPECT_FLOAT_EQ(out.g, c.g);
    EXPECT_FLOAT_EQ(out.b, c.b);
    static_assert(applySaturation(ColorFillRgb{0.8f, 0.2f, 0.5f}, SaturationParams{1.0f}) ==
                      ColorFillRgb{0.8f, 0.2f, 0.5f},
                  "saturation 1 returns the pixel unchanged");
}

// saturation == 0 collapses every channel to the pixel's luminance (Rec. 601 weights) — grayscale.
TEST(ApplySaturation, ZeroSaturationIsGreyscale) {
    const ColorFillRgb c{0.8f, 0.2f, 0.5f};
    const float        lum = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    const ColorFillRgb out = applySaturation(c, SaturationParams{0.0f});
    EXPECT_FLOAT_EQ(out.r, lum);
    EXPECT_FLOAT_EQ(out.g, lum);
    EXPECT_FLOAT_EQ(out.b, lum);
    EXPECT_FLOAT_EQ(out.r, out.g);  // every channel equal — the grayscale property
    EXPECT_FLOAT_EQ(out.g, out.b);
}

// An intermediate saturation lerps each channel linearly between the luminance (0) and the original (1):
// at 0.5 every channel sits at the midpoint of its gray and its color.
TEST(ApplySaturation, IntermediateInterpolatesLinearly) {
    const ColorFillRgb c{0.8f, 0.2f, 0.5f};
    const float        lum = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    const ColorFillRgb out = applySaturation(c, SaturationParams{0.5f});
    EXPECT_FLOAT_EQ(out.r, (c.r + lum) * 0.5f);
    EXPECT_FLOAT_EQ(out.g, (c.g + lum) * 0.5f);
    EXPECT_FLOAT_EQ(out.b, (c.b + lum) * 0.5f);
}

// A gray pixel (r == g == b) is already its own luminance, so any saturation leaves it unchanged — there is
// no color to drain.
TEST(ApplySaturation, GreyIsUnchangedAtEverySaturation) {
    const ColorFillRgb c{0.5f, 0.5f, 0.5f};
    const ColorFillRgb out = applySaturation(c, SaturationParams{0.0f});
    EXPECT_FLOAT_EQ(out.r, 0.5f);
    EXPECT_FLOAT_EQ(out.g, 0.5f);
    EXPECT_FLOAT_EQ(out.b, 0.5f);
}

// ── Baseline preservation ──────────────────────────────────────────────────────────────

// A ColorSaturation effect survives the active-effect filter (it is not None), so its pass runs.
TEST(SaturationBaseline, NoneIsDroppedSaturationSurvives) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation});
    frame.postEffects.push_back(ScreenSpaceEffect{});  // None — filtered out
    const std::vector<ScreenSpaceEffect> active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::ColorSaturation);
}

}  // namespace
}  // namespace retropp
