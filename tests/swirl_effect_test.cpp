// The Swirl effect's device-free CPU authorities: the resolved parameters (swirlParams — the public degrees
// summed and converted to the shader's radians, the clockwise-positive sign, a negative radius clamped), the
// read mirror (swirlReadPx / swirlSourceUv — the center as a fixed point, the (1 − t²)² falloff, zero twist
// at the rim, an outside-the-disc fragment reading its own coordinate exactly), the sprite footprint bound
// (spriteDisplaceBound's chord), the sprite record path (packSpriteFxRecord's resolved twist + the center on
// the gate lanes; region Swirl steps are unsupported and skipped), and the compose-skip fingerprint (every
// consulted field reprints).

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"

namespace retropp {
namespace {

constexpr float kPi = 3.14159265358979323846f;

ScreenSpaceEffect swirlAt(Point center, float radius, float amplitudeDeg, float phaseDeg = 0.0f) {
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Swirl};
    e.amplitude = amplitudeDeg;
    e.phase     = phaseDeg;
    e.center    = center;
    e.radius    = radius;
    return e;
}

// ── swirlParams — degrees in, radians out ─────────────────────────────────────────────────

TEST(SwirlParamsResolve, ConvertsDegreesToRadiansAndSumsAmplitudeWithPhase) {
    // The public surface is DEGREES (Transform::rotation's unit); the shader consumes radians. A half turn
    // authored as 180 degrees resolves to pi radians of twist.
    const SwirlParams half = swirlParams(swirlAt(Point{80.0f, 72.0f}, 40.0f, 180.0f));
    EXPECT_NEAR(std::abs(half.twist), kPi, 1e-5f);
    EXPECT_FLOAT_EQ(half.centerX, 80.0f);
    EXPECT_FLOAT_EQ(half.centerY, 72.0f);
    EXPECT_FLOAT_EQ(half.radius, 40.0f);

    // `phase` ADDS to `amplitude` in degrees — advancing it spins the vortex.
    const SwirlParams spun = swirlParams(swirlAt(Point{}, 10.0f, 120.0f, 60.0f));
    EXPECT_NEAR(std::abs(spun.twist), kPi, 1e-5f);  // 120 + 60 = 180 degrees

    // A full turn, and the degrees scale linearly.
    const SwirlParams full = swirlParams(swirlAt(Point{}, 10.0f, 360.0f));
    EXPECT_NEAR(std::abs(full.twist), 2.0f * kPi, 1e-5f);
}

TEST(SwirlParamsResolve, PositiveAmplitudeTurnsTheContentClockwise) {
    // Transform::rotation is clockwise-positive in the engine's top-left-origin pixel space; Swirl matches
    // it. The shader rotates the source-READ offset, and reading clockwise SHOWS the content turned
    // counter-clockwise — so a positive developer amplitude resolves to a NEGATIVE read rotation.
    EXPECT_LT(swirlParams(swirlAt(Point{}, 10.0f, 90.0f)).twist, 0.0f);
    EXPECT_GT(swirlParams(swirlAt(Point{}, 10.0f, -90.0f)).twist, 0.0f);

    // A point on the +x axis, turned a quarter clockwise, must be READ from the -y side (above the center
    // in a y-down space) — that is what makes the content appear rotated clockwise.
    const SwirlParams p   = swirlParams(swirlAt(Point{100.0f, 100.0f}, 50.0f, 90.0f));
    const Vec2        src = swirlReadPx(Vec2{100.0f + 1.0f, 100.0f}, p);  // just off the center, +x
    EXPECT_LT(src.y, 100.0f);
    // Near the center the falloff is ~1, so the quarter turn lands the read almost exactly on the -y axis:
    // the x offset collapses to cos(theta) ≈ 0 of the 1 px it started with.
    EXPECT_NEAR(src.x - 100.0f, 0.0f, 0.01f);
    EXPECT_NEAR(src.y - 100.0f, -1.0f, 0.01f);
}

TEST(SwirlParamsResolve, ZeroTurnAndNegativeRadiusAreTheIdentityDefaults) {
    const SwirlParams unset = swirlParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Swirl});
    EXPECT_FLOAT_EQ(unset.twist, 0.0f);   // an unset Swirl is a no-op
    EXPECT_FLOAT_EQ(unset.radius, 0.0f);

    // amplitude and phase canceling is a zero turn, not a near-zero one.
    EXPECT_FLOAT_EQ(swirlParams(swirlAt(Point{}, 10.0f, 90.0f, -90.0f)).twist, 0.0f);

    // A negative radius is no disc, never a reflected one.
    EXPECT_FLOAT_EQ(swirlParams(swirlAt(Point{}, -8.0f, 180.0f)).radius, 0.0f);
}

// ── swirlReadPx — the read mirror the shader reproduces ───────────────────────────────────

TEST(SwirlRead, TheCentreIsAFixedPoint) {
    // Rotating a zero offset moves nothing, at any twist.
    const SwirlParams p = swirlParams(swirlAt(Point{64.0f, 48.0f}, 30.0f, 270.0f));
    const Vec2 src = swirlReadPx(Vec2{64.0f, 48.0f}, p);
    EXPECT_NEAR(src.x, 64.0f, 1e-4f);
    EXPECT_NEAR(src.y, 48.0f, 1e-4f);
}

TEST(SwirlRead, OutsideTheDiscReadsItsOwnCoordinateExactly) {
    // Not "close to" — the exact same value, so an untouched pixel is byte-identical on the GPU.
    const SwirlParams p = swirlParams(swirlAt(Point{50.0f, 50.0f}, 20.0f, 200.0f));
    for (const Vec2 outside : {Vec2{50.0f + 20.0f, 50.0f},   // exactly at the rim (t == 1)
                               Vec2{50.0f + 40.0f, 50.0f},
                               Vec2{0.0f, 0.0f},
                               Vec2{50.0f, 50.0f - 25.0f}}) {
        const Vec2 src = swirlReadPx(outside, p);
        EXPECT_FLOAT_EQ(src.x, outside.x);
        EXPECT_FLOAT_EQ(src.y, outside.y);
    }
}

TEST(SwirlRead, TwistFadesToNothingAtTheRimAndIsFullAtTheCentre) {
    // theta(t) = twist·(1 − t²)² — the falloff has zero slope at BOTH ends, so there is no crease at the
    // center and no seam at the rim. Check the angle swept at a few radii against the closed form.
    const float       radius = 100.0f;
    const SwirlParams p      = swirlParams(swirlAt(Point{0.0f, 0.0f}, radius, 180.0f));
    for (const float t : {0.25f, 0.5f, 0.75f, 0.9f}) {
        const float r     = t * radius;
        const Vec2  src   = swirlReadPx(Vec2{r, 0.0f}, p);
        const float swept = std::atan2(src.y, src.x);           // the angle the read moved through
        const float f     = 1.0f - t * t;
        EXPECT_NEAR(swept, p.twist * f * f, 1e-4f) << "at t = " << t;
        // The rotation is rigid — the read stays on its own circle.
        EXPECT_NEAR(std::sqrt(src.x * src.x + src.y * src.y), r, 1e-3f) << "at t = " << t;
    }
}

TEST(SwirlRead, IdentityAtZeroTwistAndZeroRadius) {
    const Vec2 px{33.0f, 21.0f};
    const SwirlParams noTurn = swirlParams(swirlAt(Point{30.0f, 20.0f}, 50.0f, 0.0f));
    EXPECT_FLOAT_EQ(swirlReadPx(px, noTurn).x, px.x);
    EXPECT_FLOAT_EQ(swirlReadPx(px, noTurn).y, px.y);

    const SwirlParams noDisc = swirlParams(swirlAt(Point{30.0f, 20.0f}, 0.0f, 180.0f));
    EXPECT_FLOAT_EQ(swirlReadPx(px, noDisc).x, px.x);
    EXPECT_FLOAT_EQ(swirlReadPx(px, noDisc).y, px.y);
}

TEST(SwirlSourceUv, MirrorsTheShaderIncludingTheUnsnappedOutsideRead) {
    // The UV peer over a site's pixel extent. A non-Swirl kind and an outside-the-disc fragment both return
    // the ORIGINAL uv — the shader samples `uv` itself there, never the snapped evaluation point.
    const PixelSize        dims{160, 144};
    const ScreenSpaceEffect e = swirlAt(Point{80.0f, 72.0f}, 32.0f, 180.0f);

    const Uv outside{0.05f, 0.05f};
    const Uv keptSnapped = swirlSourceUv(outside, e, dims, /*snap=*/true);
    EXPECT_FLOAT_EQ(keptSnapped.u, outside.u);
    EXPECT_FLOAT_EQ(keptSnapped.v, outside.v);

    ScreenSpaceEffect notSwirl = e;
    notSwirl.kind = ScreenSpaceEffectKind::Ripple;
    const Uv inside{0.5f, 0.55f};
    const Uv passthrough = swirlSourceUv(inside, notSwirl, dims);
    EXPECT_FLOAT_EQ(passthrough.u, inside.u);
    EXPECT_FLOAT_EQ(passthrough.v, inside.v);

    // Inside the disc the UV form agrees with the px form evaluated at the same point.
    const Uv          moved = swirlSourceUv(inside, e, dims, /*snap=*/false);
    const SwirlParams p     = swirlParams(e);
    const Vec2        px    = swirlReadPx(Vec2{inside.u * 160.0f, inside.v * 144.0f}, p);
    EXPECT_NEAR(moved.u, px.x / 160.0f, 1e-6f);
    EXPECT_NEAR(moved.v, px.y / 144.0f, 1e-6f);
    EXPECT_NE(moved.u, inside.u);  // the twist is doing something
}

// ── The sprite footprint bound ────────────────────────────────────────────────────────────

TEST(SwirlSpriteBound, InflatesByTheChordAndCapsAtTwiceTheRadius) {
    // The read excursion is a CHORD: 2R·sin(theta/2) never exceeds min(theta·R, 2R). The bound uses that
    // conservative form so a twisted crest is never clipped at the static quad.
    Sprite quarter{.key = "w"};
    quarter.effects = {swirlAt(Point{8.0f, 8.0f}, 8.0f, 90.0f)};
    const detail::SpriteDisplaceBound q = detail::spriteDisplaceBound(quarter);
    EXPECT_NEAR(q.u, (kPi / 2.0f) * 8.0f, 1e-3f);  // theta·R while theta < 2 rad
    EXPECT_FLOAT_EQ(q.u, q.v);                      // radial — both axes

    Sprite full{.key = "w2"};
    full.effects = {swirlAt(Point{8.0f, 8.0f}, 8.0f, 360.0f)};
    const detail::SpriteDisplaceBound f = detail::spriteDisplaceBound(full);
    EXPECT_FLOAT_EQ(f.u, 2.0f * 8.0f);              // capped at 2R — a full turn cannot reach further

    // phase counts toward the turn, and an unset Swirl inflates nothing.
    Sprite spun{.key = "w3"};
    spun.effects = {swirlAt(Point{8.0f, 8.0f}, 8.0f, 180.0f, 180.0f)};
    EXPECT_FLOAT_EQ(detail::spriteDisplaceBound(spun).u, 2.0f * 8.0f);

    Sprite unset{.key = "w4"};
    unset.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Swirl}};
    EXPECT_FLOAT_EQ(detail::spriteDisplaceBound(unset).u, 0.0f);
}

TEST(SwirlSpriteBound, GpuSpriteTakesTheDisplacementPrePass) {
    Sprite s{.key = "vortex", .x = 20, .y = 20};
    s.size    = AssetDimensions{16, 16};
    s.effects = {swirlAt(Point{8.0f, 8.0f}, 6.0f, 180.0f)};
    EXPECT_TRUE(spriteHasDisplacement(s));
    const GpuSprite g = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    EXPECT_NE(g.flags & kSpriteHasDisplacementFlag, 0u);
    EXPECT_TRUE(spriteEmissionSteps(s, 1.0f).empty());  // a re-read radiates nothing

    // A Below-scope Swirl distorts the SCENE, so it drives no art-side displacement pre-pass.
    Sprite lens{.key = "lens"};
    lens.size       = AssetDimensions{16, 16};
    ScreenSpaceEffect below = swirlAt(Point{8.0f, 8.0f}, 6.0f, 180.0f);
    below.scope     = ScreenSpaceEffectScope::Below;
    lens.effects    = {below};
    EXPECT_FALSE(spriteHasDisplacement(lens));
}

// ── The sprite record path ────────────────────────────────────────────────────────────────

TEST(SwirlSpriteRecords, PacksTheResolvedTwistAndTheCentreOnTheGateLanes) {
    const ScreenSpaceEffect e = swirlAt(Point{6.0f, 9.0f}, 7.0f, 120.0f, 30.0f);
    const SpriteFxRecord    r = packSpriteFxRecord(e, /*isRegion=*/false, ShapePoints{}, 1.0f, BlendMode::Normal);
    const SwirlParams       p = swirlParams(e);
    EXPECT_EQ(r.kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Swirl));
    EXPECT_FLOAT_EQ(r.params[0], p.twist);   // RADIANS — the record carries what the fragment consumes
    EXPECT_FLOAT_EQ(r.params[1], p.radius);
    EXPECT_FLOAT_EQ(r.radius, p.centerX);    // the center rides the idle chain-step gate lanes
    EXPECT_FLOAT_EQ(r.strokeWidth, p.centerY);
    EXPECT_NEAR(std::abs(r.params[0]), kPi * 150.0f / 180.0f, 1e-5f);
}

TEST(SwirlSpriteRecords, RegionSwirlIsSkippedTheChainStepIsKept) {
    // A displacing kind re-reads the whole silhouette and its packed center occupies the lanes a region's
    // shape needs — so a region-confined Swirl has no packing (the renderer warns) while the chain step packs.
    Sprite s{.key = "w"};
    s.effects = {swirlAt(Point{8.0f, 8.0f}, 6.0f, 180.0f)};
    s.regions = {Region{.key = "rg", .shape = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                        .effects = {swirlAt(Point{8.0f, 8.0f}, 6.0f, 180.0f)}}};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Swirl));
    EXPECT_EQ(recs[0].flags & kSpriteFxIsRegion, 0u);

    // Same rule on the scene-facing path — and a Below-scope Swirl is a supported built-in lens.
    Sprite lens{.key = "w2"};
    ScreenSpaceEffect chain = swirlAt(Point{80.0f, 72.0f}, 40.0f, 180.0f);
    chain.scope = ScreenSpaceEffectScope::Below;
    ScreenSpaceEffect inRegion = chain;
    lens.effects = {chain};
    lens.regions = {Region{.key = "rg", .shape = ShapePoints{.points = {Point{8.0f, 8.0f}}, .radius = 6.0f},
                           .effects = {inRegion}}};
    const std::vector<SpriteFxRecord> belowRecs = buildSpriteBelowRecords(lens);
    ASSERT_EQ(belowRecs.size(), 1u);
    EXPECT_EQ(belowRecs[0].flags & kSpriteFxIsRegion, 0u);
    EXPECT_TRUE(belowSpriteKindSupported(ScreenSpaceEffectKind::Swirl));
}

TEST(SwirlSpriteRecords, EvalPassesSwirlThroughUnchanged) {
    // The color-chain oracle leaves a Swirl to the fragment's displacement pre-pass — a displacing step
    // moves WHERE the art is read, it never grades the color.
    Sprite s{.key = "w"};
    s.effects = {swirlAt(Point{8.0f, 8.0f}, 6.0f, 180.0f)};
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

TEST(SwirlSpriteRecords, ComposesWithTheOtherDisplacingKindsInChainOrder) {
    // A Swirl step joins RowDisplacement / Ripple in the same pre-pass, each re-reading the coordinate the
    // previous one produced.
    Sprite s{.key = "w"};
    s.size    = AssetDimensions{32, 32};
    s.effects = {swirlAt(Point{16.0f, 16.0f}, 12.0f, 90.0f)};
    const SpriteDisplacedRead only = spriteDisplacedRead(Uv{0.5f, 0.6f}, s);

    ScreenSpaceEffect ripple{.kind = ScreenSpaceEffectKind::Ripple};
    ripple.amplitude = 2.0f; ripple.frequency = 3.0f; ripple.center = Point{16.0f, 16.0f};
    s.effects = {swirlAt(Point{16.0f, 16.0f}, 12.0f, 90.0f), ripple};
    const SpriteDisplacedRead both = spriteDisplacedRead(Uv{0.5f, 0.6f}, s);
    EXPECT_NE(both.src.u, only.src.u);  // the ripple re-reads what the swirl produced
}

// ── The compose-skip fingerprint ──────────────────────────────────────────────────────────

TEST(SwirlFingerprint, EveryConsultedFieldReprints) {
    // A settled frame skips recompose on an equal fingerprint — so every field Swirl consults must fold.
    auto frameWith = [](ScreenSpaceEffect e) {
        FrameDrawState f;
        f.postEffects.push_back(e);
        return f;
    };
    const ScreenSpaceEffect base = swirlAt(Point{80.0f, 72.0f}, 40.0f, 180.0f);
    const std::uint64_t     h0   = hashFrameStructure(frameWith(base));
    EXPECT_EQ(h0, hashFrameStructure(frameWith(base)));  // deterministic

    ScreenSpaceEffect spun = base;
    spun.phase = 30.0f;                                  // the animation lane — the vortex must respin
    EXPECT_NE(h0, hashFrameStructure(frameWith(spun)));

    ScreenSpaceEffect turned = base;
    turned.amplitude = 200.0f;
    EXPECT_NE(h0, hashFrameStructure(frameWith(turned)));

    ScreenSpaceEffect wider = base;
    wider.radius = 64.0f;
    EXPECT_NE(h0, hashFrameStructure(frameWith(wider)));

    ScreenSpaceEffect moved = base;
    moved.center = Point{40.0f, 72.0f};
    EXPECT_NE(h0, hashFrameStructure(frameWith(moved)));
}

}  // namespace
}  // namespace retropp
