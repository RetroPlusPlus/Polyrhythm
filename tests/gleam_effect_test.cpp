#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/draw_state.h"

// The built-in gleam effect. Device-free coverage of the CPU side: the uniform resolution (gleamParams — a
// straight copy of the four Gleam fields) and the color transform the gleam.frag GPU stage mirrors
// (applyGleam: a luminance-keyed diagonal sheen band, the WHOLE contribution scaled by gain so gain == 0 is
// an exact identity). The transform is pure arithmetic — genuinely constexpr — so it is static_assert-
// testable. The live GPU path is build-compiled + dev-verified across the backends (the documented
// CI-headless boundary).

namespace retropp {
namespace {

// gleamParams is constexpr but cannot be asserted over a `constexpr ScreenSpaceEffect` (its heap-owning
// ObjectKey member makes such an object ill-formed); a constexpr wrapper builds the effect as a LOCAL
// temporary and returns the resolved params.
constexpr GleamParams gleamParamsOf(float sweep, float width, float gain, float slant) {
    ScreenSpaceEffect e{};
    e.kind  = ScreenSpaceEffectKind::Gleam;
    e.sweep = sweep;
    e.width = width;
    e.gain  = gain;
    e.slant = slant;
    return gleamParams(e);
}

// ── gleamParams — a straight copy of the four Gleam fields ─────────────────────────────

TEST(GleamParams, CopiesTheFourFields) {
    const GleamParams p = gleamParamsOf(0.6f, 0.2f, 1.5f, 0.4f);
    EXPECT_FLOAT_EQ(p.sweep, 0.6f);
    EXPECT_FLOAT_EQ(p.width, 0.2f);
    EXPECT_FLOAT_EQ(p.gain, 1.5f);
    EXPECT_FLOAT_EQ(p.slant, 0.4f);
    static_assert(gleamParamsOf(0.6f, 0.2f, 1.5f, 0.4f) ==
                      GleamParams{.sweep = 0.6f, .width = 0.2f, .gain = 1.5f, .slant = 0.4f},
                  "gleamParams copies sweep/width/gain/slant verbatim");
}

// GleamParams equality is constexpr (a plain struct, no heap).
static_assert(GleamParams{} == GleamParams{}, "default GleamParams compare equal");
static_assert(!(GleamParams{.gain = 0.5f} == GleamParams{}), "a differing field compares unequal");

// ── applyGleam — the sheen transform mirror ────────────────────────────────────────────

// gain == 0 is an EXACT identity: the pixel is returned unchanged, for any uv / color, even on the band
// crest. This is the crux — the default Gleam (gain 0) is a no-op, so an inactive gleam never perturbs the
// scene, and the "identity params -> unchanged" golden holds.
TEST(ApplyGleam, GainZeroIsIdentity) {
    const GleamParams   p{.sweep = 0.5f, .width = 0.1f, .gain = 0.0f, .slant = 0.35f};
    const ColorFillRgb  c{0.4f, 0.7f, 0.2f};
    const ColorFillRgb  out = applyGleam(c, 0.5f, 0.5f, p);  // on the band crest — still identity at gain 0
    EXPECT_FLOAT_EQ(out.r, c.r);
    EXPECT_FLOAT_EQ(out.g, c.g);
    EXPECT_FLOAT_EQ(out.b, c.b);
    static_assert(applyGleam(ColorFillRgb{0.4f, 0.7f, 0.2f}, 0.5f, 0.5f, GleamParams{.gain = 0.0f}) ==
                      ColorFillRgb{0.4f, 0.7f, 0.2f},
                  "gain 0 returns the pixel unchanged");
}

// At the band center (d == sweep -> crest == 1) with gain > 0, a non-black pixel brightens on every channel.
TEST(ApplyGleam, CrestBrightensANonBlackPixel) {
    const GleamParams  p{.sweep = 0.5f, .width = 0.25f, .gain = 1.0f, .slant = 0.0f};  // slant 0 -> axis d = u
    const ColorFillRgb c{0.5f, 0.5f, 0.5f};
    const ColorFillRgb out = applyGleam(c, 0.5f, 0.0f, p);  // u == sweep -> crest 1
    EXPECT_GT(out.r, c.r);
    EXPECT_GT(out.g, c.g);
    EXPECT_GT(out.b, c.b);
}

// Well outside the band (|d - sweep| > width), crest == 0 -> the pixel is unchanged.
TEST(ApplyGleam, OutsideTheBandIsUnchanged) {
    const GleamParams  p{.sweep = 0.5f, .width = 0.1f, .gain = 2.0f, .slant = 0.0f};
    const ColorFillRgb c{0.8f, 0.3f, 0.6f};
    const ColorFillRgb out = applyGleam(c, 0.5f + 0.2f, 0.0f, p);  // |d - sweep| = 0.2 > width -> band 0
    EXPECT_FLOAT_EQ(out.r, c.r);
    EXPECT_FLOAT_EQ(out.g, c.g);
    EXPECT_FLOAT_EQ(out.b, c.b);
}

// Slant tilts the band axis (d = u + v*slant): two pixels sharing the same d receive the same boost, and a
// pixel off that diagonal by more than `width` is unboosted.
TEST(ApplyGleam, SlantTiltsTheAxis) {
    const GleamParams  p{.sweep = 0.5f, .width = 0.2f, .gain = 1.0f, .slant = 0.5f};
    const ColorFillRgb c{0.6f, 0.6f, 0.6f};
    const ColorFillRgb onA = applyGleam(c, 0.5f, 0.0f, p);   // d = 0.5
    const ColorFillRgb onB = applyGleam(c, 0.25f, 0.5f, p);  // d = 0.25 + 0.25 = 0.5 -> same boost
    EXPECT_FLOAT_EQ(onA.r, onB.r);
    const ColorFillRgb off = applyGleam(c, 0.9f, 0.0f, p);   // d = 0.9, |0.9 - 0.5| = 0.4 > width -> unboosted
    EXPECT_LT(off.r, onA.r);
}

// Black stays black — the multiply of 0 is 0 and the white lift is luminance-keyed (lum 0 -> no lift), so
// empty space is never lit (no halo / box around the band).
TEST(ApplyGleam, BlackStaysBlack) {
    const GleamParams  p{.sweep = 0.5f, .width = 0.5f, .gain = 4.0f, .slant = 0.0f};
    const ColorFillRgb out = applyGleam(ColorFillRgb{0.0f, 0.0f, 0.0f}, 0.5f, 0.0f, p);
    EXPECT_FLOAT_EQ(out.r, 0.0f);
    EXPECT_FLOAT_EQ(out.g, 0.0f);
    EXPECT_FLOAT_EQ(out.b, 0.0f);
    static_assert(applyGleam(ColorFillRgb{0.0f, 0.0f, 0.0f}, 0.5f, 0.0f,
                             GleamParams{.sweep = 0.5f, .width = 0.5f, .gain = 4.0f, .slant = 0.0f}) ==
                      ColorFillRgb{0.0f, 0.0f, 0.0f},
                  "black is never lit — no halo around the band");
}

// ── Baseline preservation ──────────────────────────────────────────────────────────────

// A Gleam effect survives the active-effect filter (it is not None), so its pass runs.
TEST(GleamBaseline, NoneIsDroppedGleamSurvives) {
    FrameDrawState frame;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam});
    frame.postEffects.push_back(ScreenSpaceEffect{});  // None — filtered out
    const std::vector<ScreenSpaceEffect> active = activeFrameEffects(frame);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].kind, ScreenSpaceEffectKind::Gleam);
}

}  // namespace
}  // namespace retropp
