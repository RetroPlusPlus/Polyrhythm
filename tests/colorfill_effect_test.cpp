#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"

// The built-in colour-fill effect. Device-free coverage of the CPU side: the uniform resolution
// (colorFillParams — the Rgba8 fill's rgb normalized to [0,1]) and the colour transform the colorfill.frag
// GPU stage mirrors (applyColorFill: the output rgb IS the fill colour — a solid fill, opacity is the layer
// alpha). The transform is pure arithmetic — genuinely constexpr — so it is static_assert-testable. The
// live GPU path is build-compiled + dev-verified across all three backends (the documented CI-headless
// boundary).

namespace retropp {
namespace {

// colorFillParams is constexpr. It cannot be asserted over a `constexpr ScreenSpaceEffect` instance (a
// constexpr object with a heap-owning member is rejected by libstdc++); a constexpr wrapper FUNCTION builds
// the effect as a LOCAL temporary (constructed + destroyed within the constant evaluation), which is
// well-formed, and exposes the resolved params.
constexpr ColorFillParams colorFillParamsOf(Rgba8 fill, float fillIntensity = 1.0f) {
    ScreenSpaceEffect e{};
    e.kind          = ScreenSpaceEffectKind::ColorFill;
    e.fill          = fill;
    e.fillIntensity = fillIntensity;
    return colorFillParams(e);
}

// ── colorFillParams — the Rgba8 fill's rgb normalized to [0,1] ────────────────────────

// The Rgba8 rgb channels normalize 0..255 → 0..1 (the fill's alpha is not part of the stage — opacity is
// the layer alpha).
TEST(ColorFillParams, NormalizesRgb) {
    const ColorFillParams p = colorFillParamsOf(Rgba8{255, 0, 128});
    EXPECT_FLOAT_EQ(p.r, 1.0f);
    EXPECT_FLOAT_EQ(p.g, 0.0f);
    EXPECT_FLOAT_EQ(p.b, 128.0f / 255.0f);
    static_assert(colorFillParamsOf(Rgba8{255, 0, 0}).r == 1.0f, "255 normalizes to 1");
    static_assert(colorFillParamsOf(Rgba8{0, 0, 0}) == ColorFillParams{}, "black resolves to the zero params");
}

// ColorFillParams equality is constexpr (a plain struct, no heap — unlike its parent effect).
static_assert(ColorFillParams{} == ColorFillParams{}, "default ColorFillParams compare equal");
static_assert(!(ColorFillParams{.r = 0.5f} == ColorFillParams{}), "a differing field compares unequal");

// ── fillIntensity — scales the fill so it can exceed 1 (the float16-headroom brightening knob) ──

// The default intensity of 1 leaves the fill at its plain normalized value.
TEST(ColorFillIntensity, DefaultIsThePlainFill) {
    const ColorFillParams p = colorFillParamsOf(Rgba8{128, 64, 32});
    EXPECT_FLOAT_EQ(p.r, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(p.g, 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(p.b, 32.0f / 255.0f);
}

// Above 1 scales the fill past 1 — the multiplicative-exposure headroom a Multiply container brightens with
// (visible only because the offscreen intermediates are float16; an 8-bit intermediate would clamp it).
TEST(ColorFillIntensity, AboveOneScalesPastOne) {
    const ColorFillParams p = colorFillParamsOf(Rgba8{128, 128, 128}, 2.0f);
    EXPECT_FLOAT_EQ(p.r, 256.0f / 255.0f);
    EXPECT_GT(p.r, 1.0f);
    static_assert(colorFillParamsOf(Rgba8{128, 128, 128}, 2.0f).r > 1.0f,
                  "fillIntensity > 1 lifts the fill past 1 — the headroom Multiply brightening relies on");
}

// Below 1 dims the fill toward black.
TEST(ColorFillIntensity, BelowOneDimsTheFill) {
    const ColorFillParams p = colorFillParamsOf(Rgba8{200, 100, 50}, 0.5f);
    EXPECT_FLOAT_EQ(p.r, (200.0f / 255.0f) * 0.5f);
    EXPECT_FLOAT_EQ(p.g, (100.0f / 255.0f) * 0.5f);
    EXPECT_FLOAT_EQ(p.b, (50.0f / 255.0f) * 0.5f);
}

// Zero intensity resolves to the zero (black) params regardless of the fill colour.
TEST(ColorFillIntensity, ZeroIsBlack) {
    EXPECT_TRUE(colorFillParamsOf(Rgba8{255, 200, 100}, 0.0f) == ColorFillParams{});
    static_assert(colorFillParamsOf(Rgba8{255, 200, 100}, 0.0f) == ColorFillParams{},
                  "fillIntensity 0 resolves to the zero (black) params");
}

// ── applyColorFill — the solid-fill transform mirror ──────────────────────────────────

// The output rgb is exactly the fill colour — a fill is a pure source colour, so the params are all of it.
TEST(ApplyColorFill, OutputIsTheFillColour) {
    constexpr ColorFillParams p{.r = 0.9f, .g = 0.1f, .b = 0.4f};
    const ColorFillRgb out = applyColorFill(p);
    EXPECT_FLOAT_EQ(out.r, 0.9f); EXPECT_FLOAT_EQ(out.g, 0.1f); EXPECT_FLOAT_EQ(out.b, 0.4f);
    static_assert(applyColorFill(ColorFillParams{.r = 0.9f, .g = 0.1f, .b = 0.4f}) ==
                      ColorFillRgb{0.9f, 0.1f, 0.4f},
                  "the output is the fill colour");
}

// ColorFillRgb equality is constexpr.
static_assert(ColorFillRgb{} == ColorFillRgb{}, "default ColorFillRgb compare equal");
static_assert(!(ColorFillRgb{0.5f, 0.0f, 0.0f} == ColorFillRgb{}), "a differing channel compares unequal");

// ── Baseline preservation ──────────────────────────────────────────────────────────────

// The active-effect filter keeps real kinds and drops None. A ColorFill effect survives the filter, so its
// pass runs.
TEST(ColorFillBaseline, NoneIsDroppedColorFillSurvives) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill});
    frame.postEffects.push_back(ScreenSpaceEffect{});  // None — filtered out
    const std::vector<ScreenSpaceEffect> active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::ColorFill);
}

}  // namespace
}  // namespace retropp
