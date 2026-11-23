// Path walking: the PathPacing drivers, the pure sampleWalk resolver, and the game-owned PathWalker cursor.
// Entirely device-free — a curve is plain data, the arc-length table is baked once, and the resolver is a
// pure function of (table, pacing, elapsed ticks, TimingProfile, PlaybackMode), so no window or GPU is
// created. Tick numbers are pinned against the GameBoyColor cadence (ticksForDuration(100ms) == 6, exactly
// as tween_test / animation_test pin them) so a cadence regression is loud here too.

#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

#include "retropp/animation.h"  // PlaybackMode
#include "retropp/curve.h"
#include "retropp/geometry.h"
#include "retropp/path_walker.h"
#include "retropp/timing.h"
#include "retropp/tween.h"

using namespace retropp;
using namespace std::chrono_literals;

namespace {

const TimingProfile gbc = TimingProfile::GameBoyColor;

// A horizontal line from (0,0) to (100,0): length 100, atDistance(s) = (s, 0), unit facing (1, 0). Straight
// so the arc-length is exact and every expected point / heading is trivial.
ArcLengthTable line100() { return Curve::line(Vec2{0, 0}, Vec2{100, 0}).arcTable(); }

// The per-tick step the Speed driver takes on a cadence — detail::speedDistanceAt over one tick, the measure
// sampleWalk reads, so expected distances come from the same measure and compare with EXPECT_FLOAT_EQ.
float pxPerTick(float pxPerSecond, const TimingProfile& p) {
    return static_cast<float>(detail::speedDistanceAt(pxPerSecond, p, 1));
}

}  // namespace

// ── PathPacing named constructors ─────────────────────────────────────────────────────────────────────

TEST(PathPacing, SpeedCarriesTheRateAndDefaultsElsewhere) {
    const PathPacing p = PathPacing::speed(120.0f);
    EXPECT_EQ(p.kind, PathPacing::Kind::Speed);
    EXPECT_FLOAT_EQ(p.pxPerSecond, 120.0f);
    EXPECT_EQ(p.distance, nullptr);
}

TEST(PathPacing, EasedCarriesDurationAndCurve) {
    const PathPacing p = PathPacing::eased(100ms, Easing::Linear);
    EXPECT_EQ(p.kind, PathPacing::Kind::Eased);
    EXPECT_EQ(p.duration, std::chrono::nanoseconds{100ms});
    EXPECT_EQ(p.easing, Easing::Linear);
    // Default curve matches TweenSegment's default (one consistent pleasant ease across the engine).
    EXPECT_EQ(PathPacing::eased(100ms).easing, Easing::InOutQuad);
}

TEST(PathPacing, DistanceTweenCarriesThePointer) {
    const Tween<float> t = Tween<float>::of(0.0f, 100.0f, 100ms);
    const PathPacing    p = PathPacing::distanceTween(t);
    EXPECT_EQ(p.kind, PathPacing::Kind::DistanceTween);
    EXPECT_EQ(p.distance, &t);
}

TEST(PathPacing, DefaultIsParkedSpeed) {
    const PathPacing p{};
    EXPECT_EQ(p.kind, PathPacing::Kind::Speed);
    EXPECT_FLOAT_EQ(p.pxPerSecond, 0.0f);  // parked at the start
}

// ── sampleWalk — Speed ────────────────────────────────────────────────────────────────────────────────────

TEST(WalkAtSpeed, DistanceLinearInTicksWithMatchingPositionAndFacing) {
    const ArcLengthTable t   = line100();
    const PathPacing     p   = PathPacing::speed(200.0f);
    const auto           one = PlaybackMode::single();

    const WalkSample s = sampleWalk(t, p, 10, gbc, one);
    EXPECT_FLOAT_EQ(s.distance, pxPerTick(200.0f, gbc) * 10.0f);
    EXPECT_FALSE(s.finished);
    // position == atDistance(s), facing == tangentAtDistance(s) — the pair that agrees.
    EXPECT_NEAR(s.position.x, s.distance, 1e-3f);
    EXPECT_NEAR(s.position.y, 0.0f, 1e-3f);
    EXPECT_NEAR(s.facing.x, 1.0f, 1e-3f);
    EXPECT_NEAR(s.facing.y, 0.0f, 1e-3f);
}

TEST(WalkAtSpeed, LoopIndefinitelyWrapsAndNeverFinishes) {
    const ArcLengthTable t    = line100();
    const PathPacing     p    = PathPacing::speed(200.0f);
    const auto           loop = PlaybackMode::loopIndefinitely();
    const float          raw  = pxPerTick(200.0f, gbc) * 40.0f;  // > 100 → wraps
    ASSERT_GT(raw, 100.0f);

    const WalkSample s = sampleWalk(t, p, 40, gbc, loop);
    EXPECT_FLOAT_EQ(s.distance, std::fmod(raw, 100.0f));
    EXPECT_LT(s.distance, raw);
    EXPECT_FALSE(s.finished);
}

TEST(WalkAtSpeed, SingleFinishesAtLengthAndHoldsTheEndpoint) {
    const ArcLengthTable t   = line100();
    const PathPacing     p   = PathPacing::speed(200.0f);
    const auto           one = PlaybackMode::single();
    // raw crosses 100 at tick ceil(100 / pxPerTick) — tick 40 is well past it.
    const WalkSample s = sampleWalk(t, p, 40, gbc, one);
    EXPECT_TRUE(s.finished);
    EXPECT_FLOAT_EQ(s.distance, 100.0f);
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 9999, gbc, one).distance, 100.0f);  // holds forever
}

TEST(WalkAtSpeed, LoopNTimesHoldsAfterNPasses) {
    const ArcLengthTable t  = line100();
    const PathPacing     p  = PathPacing::speed(200.0f);
    const auto           n2 = PlaybackMode::loopNTimes(2);  // ends at raw >= 200
    const float          step = pxPerTick(200.0f, gbc);

    EXPECT_FALSE(sampleWalk(t, p, 59, gbc, n2).finished);   // raw ≈ 197.6 < 200
    ASSERT_LT(step * 59.0f, 200.0f);
    const WalkSample s = sampleWalk(t, p, 60, gbc, n2);     // raw ≈ 200.9 >= 200
    ASSERT_GE(step * 60.0f, 200.0f);
    EXPECT_TRUE(s.finished);
    EXPECT_FLOAT_EQ(s.distance, 100.0f);
}

TEST(WalkAtSpeed, LoopNTimesZeroRestsAtTheStartFinished) {
    const ArcLengthTable t = line100();
    const PathPacing     p = PathPacing::speed(200.0f);
    const WalkSample     s = sampleWalk(t, p, 5, gbc, PlaybackMode::loopNTimes(0));
    EXPECT_TRUE(s.finished);
    EXPECT_FLOAT_EQ(s.distance, 0.0f);  // never played → sits where it started
}

TEST(WalkAtSpeed, PlayForDurationHoldsTheCutoffDistancePastD) {
    const ArcLengthTable t   = line100();
    const PathPacing     p   = PathPacing::speed(200.0f);
    const std::uint64_t  cut = gbc.ticksForDuration(100ms);
    ASSERT_EQ(cut, 6u);
    const auto dur = PlaybackMode::playForDuration(100ms);

    EXPECT_FALSE(sampleWalk(t, p, 5, gbc, dur).finished);
    const WalkSample s = sampleWalk(t, p, 10, gbc, dur);  // past the cutoff
    EXPECT_TRUE(s.finished);
    // Distance shown at the cutoff = the last played tick (cut - 1 == 5).
    EXPECT_FLOAT_EQ(s.distance, std::fmod(pxPerTick(200.0f, gbc) * 5.0f, 100.0f));
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 500, gbc, dur).distance, s.distance);  // frozen past the cutoff
}

TEST(WalkAtSpeed, ZeroSpeedIsParkedAndTheLoopNeverFinishes) {
    const ArcLengthTable t = line100();
    const PathPacing     p = PathPacing::speed(0.0f);
    const WalkSample     s = sampleWalk(t, p, 1000, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_FLOAT_EQ(s.distance, 0.0f);
    EXPECT_FALSE(s.finished);
}

// ── sampleWalk — Eased ────────────────────────────────────────────────────────────────────────────────────

TEST(WalkAtEased, EndpointsArePinnedAndTheMidpointEases) {
    const ArcLengthTable t   = line100();
    const PathPacing     p   = PathPacing::eased(100ms, Easing::Linear);  // durTicks == 6
    const auto           one = PlaybackMode::single();

    EXPECT_FLOAT_EQ(sampleWalk(t, p, 0, gbc, one).distance, 0.0f);            // s = 0 at tick 0
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 3, gbc, one).distance, 50.0f);          // linear midpoint
    const WalkSample end = sampleWalk(t, p, 6, gbc, one);
    EXPECT_TRUE(end.finished);
    EXPECT_FLOAT_EQ(end.distance, 100.0f);                               // eased endpoint pinned exactly
}

TEST(WalkAtEased, MidpointFollowsTheEasingCurve) {
    const ArcLengthTable t = line100();
    const PathPacing     p = PathPacing::eased(100ms, Easing::InQuad);  // durTicks == 6
    // At the half-window tick (3) localT = 0.5 → InQuad 0.25 → 25.
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 3, gbc, PlaybackMode::single()).distance,
                    100.0f * ease(Easing::InQuad, 0.5f));
}

TEST(WalkAtEased, LoopWraps) {
    const ArcLengthTable t    = line100();
    const PathPacing     p    = PathPacing::eased(100ms, Easing::Linear);  // 6-tick pass
    const auto           loop = PlaybackMode::loopIndefinitely();
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 9, gbc, loop).distance, 50.0f);  // posInPass 9 % 6 == 3 → 50
    EXPECT_FALSE(sampleWalk(t, p, 9, gbc, loop).finished);
}

TEST(WalkAtEased, ZeroDurationIsInstantaneous) {
    const ArcLengthTable t = line100();
    const PathPacing     p = PathPacing::eased(8ms, Easing::Linear);  // 8ms → 0 ticks
    ASSERT_EQ(gbc.ticksForDuration(8ms), 0u);

    const WalkSample fin = sampleWalk(t, p, 0, gbc, PlaybackMode::single());
    EXPECT_TRUE(fin.finished);
    EXPECT_FLOAT_EQ(fin.distance, 100.0f);  // finite mode → immediately at the end
    const WalkSample loop = sampleWalk(t, p, 0, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_FALSE(loop.finished);            // indefinite loop rests there, never stalls
    EXPECT_FLOAT_EQ(loop.distance, 100.0f);
}

// ── sampleWalk — DistanceTween ────────────────────────────────────────────────────────────────────────────

TEST(WalkAtDistanceTween, PassesThroughTheTweenAndClampsToLength) {
    const ArcLengthTable t   = line100();
    const Tween<float>   over = Tween<float>::of(0.0f, 150.0f, 100ms, Easing::Linear);  // overshoots 100
    const PathPacing     p    = PathPacing::distanceTween(over);
    const auto           one  = PlaybackMode::single();

    EXPECT_FLOAT_EQ(sampleWalk(t, p, 3, gbc, one).distance, 75.0f);   // tween at 0.5 → 75, within [0,100]
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 6, gbc, one).distance, 100.0f);  // tween 150 clamped to length
}

TEST(WalkAtDistanceTween, BackwardMotionIsAFeature) {
    const ArcLengthTable t = line100();
    // A yoyo distance profile: 0 → 100 over 6 ticks, then 100 → 0 over 6. At tick 9 it is on the way back.
    const Tween<float> yoyo =
        Tween<float>::of(0.0f, 100.0f, 100ms, Easing::Linear).then(0.0f, 100ms, Easing::Linear);
    const PathPacing p    = PathPacing::distanceTween(yoyo);
    const auto       loop = PlaybackMode::loopIndefinitely();

    const float peak = sampleWalk(t, p, 6, gbc, loop).distance;   // the top of the path
    const float back = sampleWalk(t, p, 9, gbc, loop).distance;   // halfway back down
    EXPECT_FLOAT_EQ(peak, 100.0f);
    EXPECT_FLOAT_EQ(back, 50.0f);
    EXPECT_LT(back, peak);  // it moved BACKWARD along the path — the distance-tween form's capability
}

TEST(WalkAtDistanceTween, NullPointerIsTheParkedDefault) {
    const ArcLengthTable t = line100();
    const PathPacing     p{.kind = PathPacing::Kind::DistanceTween, .distance = nullptr};
    EXPECT_TRUE(sampleWalk(t, p, 5, gbc, PlaybackMode::single()).finished);           // finite → done
    EXPECT_FLOAT_EQ(sampleWalk(t, p, 5, gbc, PlaybackMode::single()).distance, 0.0f);  // parked at 0
    EXPECT_FALSE(sampleWalk(t, p, 5, gbc, PlaybackMode::loopIndefinitely()).finished); // loop → not
}

// ── sampleWalk — degenerate geometry + facing ─────────────────────────────────────────────────────────────

TEST(WalkAtDegenerate, ZeroLengthCurveHasNoTravelAndNoFacing) {
    const ArcLengthTable zero = Curve::line(Vec2{5, 5}, Vec2{5, 5}).arcTable();  // length 0
    ASSERT_FLOAT_EQ(zero.length(), 0.0f);
    const WalkSample s = sampleWalk(zero, PathPacing::speed(200.0f), 10, gbc, PlaybackMode::single());
    EXPECT_FLOAT_EQ(s.distance, 0.0f);
    EXPECT_EQ(s.facing, Vec2{});  // zero facing straight from the stateless resolver
    EXPECT_TRUE(s.finished);      // finite mode
}

TEST(WalkAtDegenerate, EmptyTableLoopIsNeverFinished) {
    const ArcLengthTable empty{};  // no segments
    const WalkSample     s = sampleWalk(empty, PathPacing::speed(50.0f), 3, gbc,
                                    PlaybackMode::loopIndefinitely());
    EXPECT_EQ(s.position, Vec2{});
    EXPECT_EQ(s.facing, Vec2{});
    EXPECT_FALSE(s.finished);
}

TEST(WalkAtFacing, IsTheUnitTangentAtTheResolvedDistance) {
    // A vertical line: facing is (0, 1). Confirms facing tracks travel direction, not just the x-axis case.
    const ArcLengthTable down = Curve::line(Vec2{0, 0}, Vec2{0, 100}).arcTable();
    const WalkSample     s    = sampleWalk(down, PathPacing::speed(200.0f), 5, gbc, PlaybackMode::single());
    EXPECT_NEAR(s.facing.x, 0.0f, 1e-3f);
    EXPECT_NEAR(s.facing.y, 1.0f, 1e-3f);
}

// ── PathWalker cursor ─────────────────────────────────────────────────────────────────────────────────

TEST(PathWalker, BareAdvanceAccruesOnlyWhilePlaying) {
    PathWalker w{.table = line100(), .pacing = PathPacing::speed(200.0f)};
    const auto loop = PlaybackMode::loopIndefinitely();

    w.advance(loop, 3);
    EXPECT_EQ(w.elapsedTicks, 3u);
    EXPECT_FLOAT_EQ(w.distance(), pxPerTick(200.0f, gbc) * 3.0f);

    w.pause();
    w.advance(loop, 100);  // frozen — elapsed does not grow
    EXPECT_EQ(w.elapsedTicks, 3u);

    w.play();
    w.advance(loop, 3);
    EXPECT_EQ(w.elapsedTicks, 6u);
}

TEST(PathWalker, StopRewindsToTheStartAndPauses) {
    PathWalker w{.table = line100(), .pacing = PathPacing::speed(200.0f)};
    w.advance(PlaybackMode::loopIndefinitely(), 5);
    ASSERT_GT(w.distance(), 0.0f);

    w.stop();
    EXPECT_FLOAT_EQ(w.distance(), 0.0f);  // rewound to the path start
    EXPECT_EQ(w.elapsedTicks, 0u);
    EXPECT_FALSE(w.playing);
    EXPECT_FALSE(w.finished());
    w.advance(PlaybackMode::loopIndefinitely(), 3);  // paused → no movement
    EXPECT_FLOAT_EQ(w.distance(), 0.0f);
}

TEST(PathWalker, RestartRewindsAndPlays) {
    PathWalker w{.table = line100(), .pacing = PathPacing::speed(200.0f)};
    w.advance(PlaybackMode::single(), 60);
    ASSERT_TRUE(w.finished());

    w.restart();
    EXPECT_FLOAT_EQ(w.distance(), 0.0f);
    EXPECT_EQ(w.elapsedTicks, 0u);
    EXPECT_TRUE(w.playing);
    EXPECT_FALSE(w.finished());
}

TEST(PathWalker, SeekLandsTheDistanceAndKeepsPlayState) {
    PathWalker w{.table = line100(), .pacing = PathPacing::speed(200.0f)};
    w.seek(100ms);  // 100ms → 6 ticks
    EXPECT_EQ(w.elapsedTicks, 6u);
    EXPECT_FLOAT_EQ(w.distance(), std::fmod(pxPerTick(200.0f, gbc) * 6.0f, 100.0f));
    EXPECT_TRUE(w.playing);  // seek preserves the play state
}

TEST(PathWalker, HoldsLastFacingOntoADeadSpot) {
    PathWalker w{.table = line100(), .pacing = PathPacing::speed(200.0f)};
    w.advance(PlaybackMode::loopIndefinitely(), 3);
    ASSERT_NEAR(w.facing().x, 1.0f, 1e-3f);  // a real heading was seen

    w.table = ArcLengthTable{};              // re-path onto a directionless (empty) table
    w.advance(PlaybackMode::loopIndefinitely(), 1);
    EXPECT_NEAR(w.facing().x, 1.0f, 1e-3f);  // the last real heading is held, not zeroed
    EXPECT_NEAR(w.facing().y, 0.0f, 1e-3f);
}

TEST(PathWalker, BareWalkerFacingIsHonestlyZero) {
    PathWalker w{};  // no path, no heading ever seen
    EXPECT_EQ(w.facing(), Vec2{});
    w.advance();  // still nothing to face
    EXPECT_EQ(w.facing(), Vec2{});
}

TEST(PathWalker, DefaultTimingIsConfigurableAndInherited) {
    const TimingProfile saved = PathWalker::defaultTiming;
    PathWalker::defaultTiming = TimingProfile{TickPeriodNs::Hz60};

    PathWalker w{.table = line100()};  // bare — inherits the configured default, not GBC
    EXPECT_EQ(w.profile, PathWalker::defaultTiming);
    EXPECT_EQ(w.profile.tickPeriodNs, TickPeriodNs::Hz60);

    PathWalker o{.table = line100(), .profile = TimingProfile::GameBoy};  // explicit override
    EXPECT_EQ(o.profile, TimingProfile::GameBoy);

    PathWalker::defaultTiming = saved;  // restore (process-wide)
    PathWalker r{};
    EXPECT_EQ(r.profile, saved);
}
