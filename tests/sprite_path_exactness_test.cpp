#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "retropp/animation.h"    // PlaybackMode
#include "retropp/curve.h"        // Curve, ArcLengthTable
#include "retropp/geometry.h"     // Vec2
#include "retropp/path_walker.h"  // PathPacing, PathWalker, sampleWalk, detail::speedDistanceAt
#include "retropp/sprite_path.h"
#include "retropp/timing.h"
#include "retropp/tween.h"        // Easing

namespace retropp {
namespace {

// A sprite path at its seams: where a detour hands the route back, the tick a constant-speed leg finishes on,
// what a path reports before it is advanced, and where a looping route is on the tick a lap completes. Every
// check is a headless value comparison; no GPU.

constexpr TimingProfile kHz60{.tickPeriodNs = TickPeriodNs::Hz60};

// A leg paced linearly over a whole number of ticks, so every position on it is a whole pixel per tick.
PathPacing ticks(int n) { return PathPacing::eased(n * kHz60.tickPeriod(), Easing::Linear); }

void expectAt(Vec2 got, Vec2 want, float eps = 1e-3f) {
    EXPECT_NEAR(got.x, want.x, eps);
    EXPECT_NEAR(got.y, want.y, eps);
}

// A base route east from (16, 16) to (64, 16) at one pixel a tick, advanced 8 ticks: the sprite is at (24, 16).
SpritePath eastAfterEightTicks() {
    SpritePath p{.nodes   = {{.move = SpritePathMove::to({64.0f, 16.0f}), .pacing = ticks(48)}},
                 .start   = {16.0f, 16.0f},
                 .profile = kHz60};
    p.advance(PlaybackMode::single(), 8);
    return p;
}

// A detour from (24, 16) down to (24, 24) over 8 ticks.
std::vector<SpritePathNode> detourDown() {
    return {{.move = SpritePathMove::to({24.0f, 24.0f}), .pacing = ticks(8)}};
}

// A path that has had no call: the route east from (16, 16) over 48 ticks.
SpritePath freshEast() {
    return SpritePath{.nodes   = {{.move = SpritePathMove::to({64.0f, 16.0f}), .pacing = ticks(48)}},
                      .start   = {16.0f, 16.0f},
                      .profile = kHz60};
}

// A straight line east from the origin, `length` pixels long.
ArcLengthTable lineEast(float length) { return Curve::line(Vec2{0.0f, 0.0f}, Vec2{length, 0.0f}).arcTable(); }

// The first tick, stepping from 1, at which `done` holds; 0 when it never does within `limit` ticks.
template <typename Done>
std::uint64_t firstTick(std::uint64_t limit, Done done) {
    for (std::uint64_t t = 1; t <= limit; ++t) {
        if (done(t)) return t;
    }
    return 0;
}

// ── A detour hands the route back ───────────────────────────────────────────────────────────────────

TEST(SpritePathResume, ContinueTickByTickLandsOnTheDetoursEnd) {
    SpritePath p = eastAfterEightTicks();
    p.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);
    for (int tick = 1; tick <= 7; ++tick) {
        p.advance(PlaybackMode::single());
        expectAt(p.position(), {24.0f, 16.0f + static_cast<float>(tick)});
        EXPECT_TRUE(p.interrupted()) << "tick " << tick;
    }
    p.advance(PlaybackMode::single());  // tick 8: the detour finishes and the route resumes on it
    expectAt(p.position(), {24.0f, 24.0f});
    EXPECT_FALSE(p.interrupted());
    p.advance(PlaybackMode::single());
    expectAt(p.position(), {25.0f, 24.0f});  // the route carries the detour's whole 8 px
    p.advance(PlaybackMode::single());
    expectAt(p.position(), {26.0f, 24.0f});
}

TEST(SpritePathResume, ContinueBatchedEqualsTickByTick) {
    SpritePath batched = eastAfterEightTicks();
    batched.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);
    SpritePath stepped = eastAfterEightTicks();
    stepped.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);

    batched.advance(PlaybackMode::single(), 8);
    for (int i = 0; i < 8; ++i) stepped.advance(PlaybackMode::single());
    expectAt(batched.position(), {24.0f, 24.0f});
    expectAt(batched.position(), stepped.position());

    batched.advance(PlaybackMode::single());
    stepped.advance(PlaybackMode::single());
    expectAt(batched.position(), {25.0f, 24.0f});
    expectAt(batched.position(), stepped.position());
}

TEST(SpritePathResume, ContinueBatchedPastTheEndEqualsTickByTick) {
    SpritePath batched = eastAfterEightTicks();
    batched.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);
    SpritePath stepped = eastAfterEightTicks();
    stepped.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);

    batched.advance(PlaybackMode::single(), 10);
    for (int i = 0; i < 10; ++i) stepped.advance(PlaybackMode::single());
    expectAt(batched.position(), {26.0f, 24.0f});
    expectAt(batched.position(), stepped.position());
    EXPECT_EQ(batched.elapsedTicks, stepped.elapsedTicks);
}

TEST(SpritePathResume, NestedContinueShiftsByTheSumOfBothDisplacements) {
    // The inner detour runs east (32, 16) over 4 ticks (+8, 0); the outer one then runs its 8 px down (0, +8).
    const std::vector<SpritePathNode> east{{.move = SpritePathMove::to({32.0f, 16.0f}), .pacing = ticks(4)}};

    SpritePath batched = eastAfterEightTicks();
    batched.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);
    batched.interrupt(east, PlaybackMode::single(), ResumePolicy::Continue);
    SpritePath stepped = eastAfterEightTicks();
    stepped.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Continue);
    stepped.interrupt(east, PlaybackMode::single(), ResumePolicy::Continue);

    batched.advance(PlaybackMode::single(), 20);
    for (int i = 0; i < 20; ++i) stepped.advance(PlaybackMode::single());
    EXPECT_EQ(batched.interruptDepth(), 0u);
    expectAt(batched.position(), {40.0f, 24.0f});  // the base at its tick 16, (32, 16), shifted by (8, 8)
    EXPECT_EQ(stepped.interruptDepth(), 0u);
    expectAt(stepped.position(), batched.position());
}

TEST(SpritePathResume, ReturnBatchedEqualsTickByTickOnTheBaseLine) {
    SpritePath batched = eastAfterEightTicks();
    batched.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Return);
    SpritePath stepped = eastAfterEightTicks();
    stepped.interrupt(detourDown(), PlaybackMode::single(), ResumePolicy::Return);

    batched.advance(PlaybackMode::single(), 8);
    for (int i = 0; i < 8; ++i) stepped.advance(PlaybackMode::single());
    expectAt(batched.position(), {24.0f, 16.0f});
    expectAt(stepped.position(), batched.position());

    batched.advance(PlaybackMode::single());
    stepped.advance(PlaybackMode::single());
    expectAt(batched.position(), {25.0f, 16.0f});
    expectAt(stepped.position(), batched.position());
}

TEST(SpritePathResume, PopInterruptHandsBackFromWhereTheDetourStands) {
    SpritePath p = eastAfterEightTicks();
    p.interrupt(detourDown(), PlaybackMode::loopIndefinitely(), ResumePolicy::Continue);
    p.advance(PlaybackMode::single(), 4);
    expectAt(p.position(), {24.0f, 20.0f});
    p.popInterrupt();
    expectAt(p.position(), {24.0f, 20.0f});
    EXPECT_FALSE(p.interrupted());
    p.advance(PlaybackMode::single());
    expectAt(p.position(), {25.0f, 20.0f});
}

// ── A constant-speed leg finishes on the step that reaches its length ───────────────────────────────

TEST(SpritePathSpeedLeg, AWholeStepLegFinishesOnTheStepThatReachesItsLength) {
    // At 60 px/s on a 60 Hz tick a step is one pixel, so the 24 px leg `a` takes 24 ticks.
    SpritePath p{.nodes   = {{.label  = "a",
                              .move   = SpritePathMove::to({40.0f, 16.0f}),
                              .pacing = PathPacing::speed(60.0f)},
                             {.label  = "b",
                              .move   = SpritePathMove::to({40.0f, 40.0f}),
                              .pacing = PathPacing::speed(60.0f)}},
                 .start   = {16.0f, 16.0f},
                 .profile = kHz60};
    for (int tick = 1; tick <= 26; ++tick) {
        p.advance(PlaybackMode::single());
        ASSERT_NE(p.currentNode(), nullptr);
        if (tick == 23) {
            EXPECT_EQ(p.currentNode()->label, "a");
            expectAt(p.position(), {39.0f, 16.0f});
        } else if (tick == 24) {
            EXPECT_EQ(p.currentNode()->label, "b");
            expectAt(p.position(), {40.0f, 16.0f});
        } else if (tick == 25) {
            expectAt(p.position(), {40.0f, 17.0f});
        } else if (tick == 26) {
            expectAt(p.position(), {40.0f, 18.0f});
        }
    }

    SpritePath alone{.nodes   = {{.label  = "a",
                                  .move   = SpritePathMove::to({40.0f, 16.0f}),
                                  .pacing = PathPacing::speed(60.0f)}},
                     .start   = {16.0f, 16.0f},
                     .profile = kHz60};
    EXPECT_EQ(firstTick(100,
                        [&](std::uint64_t) {
                            alone.advance(PlaybackMode::single());
                            return alone.finished();
                        }),
              24u);
}

TEST(SpritePathSpeedLeg, IntegerNanosecondCadencesCoverOneStepATick) {
    struct Cadence {
        TimingProfile profile;
        float         pxPerSecond;
    };
    const Cadence cadences[] = {
        {.profile = TimingProfile{.tickPeriodNs = static_cast<TickPeriodNs>(20'000'000)}, .pxPerSecond = 50.0f},
        {.profile = TimingProfile{.tickPeriodNs = static_cast<TickPeriodNs>(10'000'000)}, .pxPerSecond = 100.0f},
    };
    const ArcLengthTable table = lineEast(24.0f);
    for (const Cadence& c : cadences) {
        const PathPacing pacing = PathPacing::speed(c.pxPerSecond);
        SpritePath p{.nodes   = {{.move = SpritePathMove::to({24.0f, 0.0f}), .pacing = pacing}},
                     .start   = {0.0f, 0.0f},
                     .profile = c.profile};
        p.advance(PlaybackMode::single(), 23);
        EXPECT_FALSE(p.finished()) << c.pxPerSecond;
        p.advance(PlaybackMode::single());
        EXPECT_TRUE(p.finished()) << c.pxPerSecond;

        EXPECT_FALSE(sampleWalk(table, pacing, 23, c.profile, PlaybackMode::single()).finished);
        const WalkSample end = sampleWalk(table, pacing, 24, c.profile, PlaybackMode::single());
        EXPECT_TRUE(end.finished);
        EXPECT_FLOAT_EQ(end.distance, 24.0f);
        EXPECT_FLOAT_EQ(sampleWalk(table, pacing, 12, c.profile, PlaybackMode::single()).distance, 12.0f);
    }
}

TEST(SpritePathSpeedLeg, AFractionalLegTakesTheNextWholeTick) {
    const TimingProfile gbc = TimingProfile::GameBoyColor;
    const auto finishTick = [&](float pxPerSecond) {
        SpritePath p{.nodes   = {{.move = SpritePathMove::to({24.0f, 0.0f}), .pacing = PathPacing::speed(pxPerSecond)}},
                     .start   = {0.0f, 0.0f},
                     .profile = gbc};
        return firstTick(100, [&](std::uint64_t) {
            p.advance(PlaybackMode::single());
            return p.finished();
        });
    };
    EXPECT_EQ(finishTick(60.0f), 24u);    // 23.89 ticks of travel
    EXPECT_EQ(finishTick(59.5f), 25u);    // 24.09 ticks of travel
    EXPECT_EQ(finishTick(59.7275f), 25u); // a step of 0.99999999 px: 24 steps fall a hair short of 24 px
    const double atTick24 = detail::speedDistanceAt(59.7275f, gbc, 24);
    EXPECT_LT(atTick24, 24.0);
    EXPECT_GT(atTick24, 23.9999);
}

TEST(SpritePathSpeedLeg, TheWalkerAndTheSequenceAgreeOnEveryLegsLastTick) {
    const TimingProfile profiles[] = {kHz60, TimingProfile::GameBoyColor, TimingProfile::SnesPal};
    const float         speeds[]   = {30.0f, 45.0f, 60.0f, 59.7275f, 120.0f, 333.3f};
    const float         lengths[]  = {24.0f, 40.0f, 100.0f, 7.5f};
    for (const TimingProfile& profile : profiles) {
        for (const float speed : speeds) {
            for (const float length : lengths) {
                const PathPacing     pacing = PathPacing::speed(speed);
                const ArcLengthTable table  = lineEast(length);

                SpritePath one{.nodes   = {{.move = SpritePathMove::to({length, 0.0f}), .pacing = pacing}},
                               .start   = {0.0f, 0.0f},
                               .profile = profile};
                SpritePath two{.nodes   = {{.move = SpritePathMove::to({length, 0.0f}), .pacing = pacing},
                                           {.move = SpritePathMove::to({length, 10.0f}), .pacing = pacing}},
                               .start   = {0.0f, 0.0f},
                               .profile = profile};

                const std::uint64_t walker = firstTick(2000, [&](std::uint64_t t) {
                    return sampleWalk(table, pacing, t, profile, PlaybackMode::single()).finished;
                });
                const std::uint64_t finished = firstTick(2000, [&](std::uint64_t) {
                    one.advance(PlaybackMode::single());
                    return one.finished();
                });
                const std::uint64_t nextLeg = firstTick(2000, [&](std::uint64_t) {
                    two.advance(PlaybackMode::single());
                    return two.currentNodeIndex() == 1;
                });
                ASSERT_NE(walker, 0u);
                EXPECT_EQ(finished, walker) << "period " << profile.tickPeriod().count() << " ns, " << speed
                                            << " px/s, " << length << " px";
                EXPECT_EQ(nextLeg, walker) << "period " << profile.tickPeriod().count() << " ns, " << speed
                                           << " px/s, " << length << " px";
            }
        }
    }
}

TEST(SpritePathSpeedLeg, ALoopingSpeedWalkShowsTheStartOnTheLapTick) {
    PathWalker w{.table = lineEast(24.0f), .pacing = PathPacing::speed(60.0f), .profile = kHz60};
    for (int tick = 1; tick <= 25; ++tick) {
        w.advance();
        if (tick == 23) EXPECT_NEAR(w.distance(), 23.0f, 1e-3f);
        if (tick == 24) EXPECT_LT(w.distance(), 1e-4f);  // the next lap's start
        if (tick == 25) EXPECT_NEAR(w.distance(), 1.0f, 1e-3f);
    }

    const WalkSample before = sampleWalk(w.table, w.pacing, 47, kHz60, PlaybackMode::loopNTimes(2));
    EXPECT_FALSE(before.finished);
    const WalkSample after = sampleWalk(w.table, w.pacing, 48, kHz60, PlaybackMode::loopNTimes(2));
    EXPECT_TRUE(after.finished);
    EXPECT_FLOAT_EQ(after.distance, 24.0f);
}

// ── A path before it is advanced ────────────────────────────────────────────────────────────────────

TEST(SpritePathFreshPath, PositionBeforeAnyCallIsTheStart) {
    SpritePath p = freshEast();
    EXPECT_EQ(p.position(), (Vec2{16.0f, 16.0f}));
    EXPECT_FALSE(p.interrupted());
    EXPECT_FALSE(p.finished());
    EXPECT_FALSE(p.baked);

    p.advance(PlaybackMode::single(), 0);
    EXPECT_EQ(p.position(), (Vec2{16.0f, 16.0f}));
    EXPECT_TRUE(p.baked);
}

TEST(SpritePathFreshPath, InterruptBeforeTheFirstAdvanceDepartsFromTheStart) {
    SpritePath p = freshEast();
    p.interrupt({{.move = SpritePathMove::to({16.0f, 24.0f}), .pacing = ticks(8)}}, PlaybackMode::single(),
                ResumePolicy::Return);
    expectAt(p.position(), {16.0f, 16.0f});
    p.advance(PlaybackMode::single(), 4);
    expectAt(p.position(), {16.0f, 20.0f});  // halfway between the start and the detour's end
    p.advance(PlaybackMode::single(), 4);
    EXPECT_FALSE(p.interrupted());
    expectAt(p.position(), {16.0f, 16.0f});  // the detour has finished: Return puts the sprite back at the start
    p.advance(PlaybackMode::single());
    expectAt(p.position(), {17.0f, 16.0f});

    // A route whose first leg names its own origin begins there, and a detour pushed before the first advance
    // leaves from it.
    SpritePath named{.nodes   = {{.move = SpritePathMove::to({20.0f, 16.0f}, {64.0f, 16.0f}), .pacing = ticks(44)}},
                     .start   = {16.0f, 16.0f},
                     .profile = kHz60};
    named.interrupt({{.move = SpritePathMove::to({20.0f, 24.0f}), .pacing = ticks(8)}}, PlaybackMode::single(),
                    ResumePolicy::Return);
    expectAt(named.position(), {20.0f, 16.0f});
    named.advance(PlaybackMode::single(), 4);
    expectAt(named.position(), {20.0f, 20.0f});
}

// ── A lap ────────────────────────────────────────────────────────────────────────────────────────────

TEST(SpritePathLap, AnOpenLoopingRouteIsAtItsStartOnTheLapTick) {
    const auto route = [] {
        return SpritePath{.nodes   = {{.move = SpritePathMove::to({24.0f, 16.0f}), .pacing = ticks(8)}},
                          .start   = {16.0f, 16.0f},
                          .profile = kHz60};
    };
    SpritePath looping = route();
    for (int tick = 1; tick <= 9; ++tick) {
        looping.advance();
        if (tick == 7) expectAt(looping.position(), {23.0f, 16.0f});
        if (tick == 8) expectAt(looping.position(), {16.0f, 16.0f});  // the lap completes: the next one's start
        if (tick == 9) expectAt(looping.position(), {17.0f, 16.0f});
    }

    SpritePath once = route();
    once.advance(PlaybackMode::single(), 8);
    expectAt(once.position(), {24.0f, 16.0f});
    EXPECT_TRUE(once.finished());
}

}  // namespace
}  // namespace retropp
