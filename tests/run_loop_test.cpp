#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include "retropp/frame_timing.h"
#include "retropp/input.h"
#include "retropp/interpolation.h"
#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "manual_clock.h"

using retropp::InputSample;
using retropp::InputState;
using retropp::actionId;
using retropp::DrawLayer;
using retropp::FrameDrawState;
using retropp::FrameTiming;
using retropp::Interpolator;
using retropp::RunLoop;
using retropp::TickPeriodNs;
using retropp::frameTiming;
using retropp::TimingProfile;
using retropp::kMaxFrameTime;
using retropp::test::ManualClock;

namespace {

// The default-profile tick period (GBC, 16'742'706 ns) — the cadence these cases assert on,
// read from the profile. The default RunLoop uses this.
constexpr auto kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

// Common fixture: a manual clock + loop + counters the tests assert against.
struct LoopHarness {
    ManualClock clock;
    RunLoop loop{clock};
    int ticks = 0;
    int renders = 0;
    float lastAlpha = -1.0f;

    LoopHarness() {
        loop.simTick([this](const InputState&) { ++ticks; });
        loop.renderLoop([this] { ++renders; lastAlpha = frameTiming().alpha; });
    }

    // Settle the baseline (first advance runs zero ticks) so subsequent advances
    // measure elapsed from a known zero.
    void settle() { loop.advance(); }
};

}  // namespace

TEST(RunLoop, FirstAdvanceEstablishesBaselineAndRunsZeroTicks) {
    LoopHarness h;
    h.clock.set(std::chrono::nanoseconds{123'456});  // arbitrary nonzero baseline
    h.loop.advance();
    EXPECT_EQ(h.ticks, 0);
    EXPECT_EQ(h.loop.tickCount(), 0u);
    EXPECT_EQ(h.renders, 1);  // render still fires (alpha 0) on the baseline frame
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.0f);
}

TEST(RunLoop, ExactMultipleOfPeriodRunsExactlyThatManyTicks) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(kTickPeriod * 5);
    h.loop.advance();
    EXPECT_EQ(h.ticks, 5);
    EXPECT_EQ(h.loop.tickCount(), 5u);
    // Landing exactly on a tick boundary leaves no sub-tick fraction, but the iteration committed
    // five ticks, so the mirror spans five fixed steps and the factor places the render one step
    // back from the current state: (5 - 1 + 0) / 5.
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.8f);
}

TEST(RunLoop, SteadyStateRunsOneTickPerOneFrameAdvance) {
    // One GB-frame of elapsed per advance runs exactly one tick per advance — the
    // steady-state path a real host drives. Count is rate-agnostic (100 frames → 100
    // ticks); a single multi-frame advance would instead hit the per-advance
    // spiral-of-death clamp, which is per-advance by design.
    LoopHarness h;
    h.settle();
    constexpr int kFrames = 100;
    for (int i = 0; i < kFrames; ++i) {
        h.clock.advanceBy(kTickPeriod);  // one real GB frame (59.7275 Hz), not 1/60 s
        h.loop.advance();
    }
    EXPECT_EQ(h.ticks, kFrames);
    EXPECT_EQ(h.loop.tickCount(), static_cast<std::uint64_t>(kFrames));
}

TEST(RunLoop, AlphaStaysInUnitIntervalAcrossSubPeriodAdvances) {
    LoopHarness h;
    h.settle();
    // Advance by 1.5 periods: one tick fires, half a period of residual → alpha ≈ 0.5.
    h.clock.advanceBy(kTickPeriod + kTickPeriod / 2);
    h.loop.advance();
    EXPECT_EQ(h.ticks, 1);
    EXPECT_GE(h.lastAlpha, 0.0f);
    EXPECT_LT(h.lastAlpha, 1.0f);
    EXPECT_NEAR(h.lastAlpha, 0.5f, 0.01f);
}

TEST(RunLoop, AccumulatesTickCountAcrossAdvances) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(kTickPeriod * 3);
    h.loop.advance();
    h.clock.advanceBy(kTickPeriod * 4);
    h.loop.advance();
    EXPECT_EQ(h.loop.tickCount(), 7u);
}

TEST(RunLoop, SpiralOfDeathClampCapsTicksForOverlongFrame) {
    LoopHarness h;
    h.settle();
    h.clock.advanceBy(std::chrono::seconds{10});  // far over the clamp
    h.loop.advance();
    // Capped at ⌊kMaxFrameTime / kTickPeriod⌋ — derived, not hardcoded.
    const auto cap = static_cast<int>(kMaxFrameTime / kTickPeriod);
    EXPECT_EQ(h.ticks, cap);
}

TEST(RunLoop, BackwardsClockRunsZeroTicksAndDoesNotUnderflow) {
    LoopHarness h;
    h.clock.set(std::chrono::seconds{5});
    h.settle();
    h.clock.set(std::chrono::seconds{1});  // time went backward
    h.loop.advance();
    EXPECT_EQ(h.ticks, 0);
    EXPECT_FLOAT_EQ(h.lastAlpha, 0.0f);  // accumulator unchanged, still on a boundary
}

TEST(RunLoop, RenderFiresExactlyOncePerAdvance) {
    LoopHarness h;
    h.settle();  // render #1
    h.clock.advanceBy(kTickPeriod * 3);
    h.loop.advance();  // render #2 — one render despite three ticks
    EXPECT_EQ(h.renders, 2);
}

TEST(RunLoop, CatchUpBatchSharesOneRawSampleSoEdgeFiresOnFirstTickOnly) {
    enum class Act : std::uint8_t { Fire };
    LoopHarness h;
    int pressedCount = 0;
    h.loop.simTick([&](const InputState& in) {
        if (in.justPressed(Act::Fire)) ++pressedCount;
    });
    h.settle();
    InputSample down;
    down.players[0].held.set(actionId(Act::Fire), true);
    h.loop.setRawInput(down);
    h.clock.advanceBy(kTickPeriod * 4);  // 4 ticks in one advance, same raw the whole batch
    h.loop.advance();
    EXPECT_EQ(pressedCount, 1);  // pressed edge only on the first tick of the batch
}

TEST(RunLoop, RunStopsWhenCallbackRequestsStop) {
    LoopHarness h;
    int iterations = 0;
    // Stop from inside the render callback after a few iterations; run() must return.
    h.loop.renderLoop([&] {
        if (++iterations >= 3) h.loop.stop();
    });
    h.loop.run();
    EXPECT_GE(iterations, 3);
}

TEST(RunLoop, NonDefaultProfileTicksOnItsOwnPeriod) {
    // A loop built with a non-default profile schedules on THAT period, proving the profile's
    // tick period actually threads through advance() (not the GBC default).
    ManualClock clock;
    RunLoop loop{clock, TimingProfile{TickPeriodNs::Hz60}};
    EXPECT_EQ(loop.tickPeriod(), std::chrono::nanoseconds{16'666'667});

    int ticks = 0;
    loop.simTick([&](const InputState&) { ++ticks; });
    loop.advance();                              // settle baseline
    clock.advanceBy(loop.tickPeriod() * 3);      // exactly three Hz60 periods
    loop.advance();
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(loop.tickCount(), 3u);
}

TEST(RunLoop, DefaultProfileIsGameBoyColorCadence) {
    ManualClock clock;
    RunLoop loop{clock};
    EXPECT_EQ(loop.tickPeriod(), TimingProfile::GameBoyColor.tickPeriod());
    EXPECT_EQ(loop.timing().tickPeriodNs, TickPeriodNs::GameBoyColor);
}

// ── The published blend factor spans the ticks the commit actually ran ──────────────────

namespace {

// Drives the loop one iteration at a time with explicit frame deltas, recording what each iteration
// committed and published. `expectedRaw` recomputes the bare accumulator fraction from the driven
// timeline rather than from the loop, so the assertions do not restate the implementation.
struct SpanHarness {
    ManualClock              clock;
    RunLoop                  loop{clock};
    std::chrono::nanoseconds now{};
    std::uint64_t            prevTicks = 0;
    int                      lastTicks = 0;
    float                    alpha     = -1.0f;

    SpanHarness() {
        loop.simTick([](const InputState&) {});
        loop.renderLoop([this] { alpha = frameTiming().alpha; });
        clock.set(now);
        loop.advance();  // baseline iteration — establishes the timeline, commits nothing
        prevTicks = loop.tickCount();
    }

    void step(std::chrono::nanoseconds dt) {
        now += dt;
        clock.set(now);
        loop.advance();
        lastTicks = static_cast<int>(loop.tickCount() - prevTicks);
        prevTicks = loop.tickCount();
    }

    [[nodiscard]] float expectedRaw() const {
        const auto consumed = kTickPeriod * static_cast<long long>(loop.tickCount());
        return static_cast<float>((now - consumed).count())
             / static_cast<float>(kTickPeriod.count());
    }
};

constexpr std::chrono::nanoseconds msec(double v) {
    return std::chrono::nanoseconds{static_cast<long long>(v * 1'000'000.0)};
}

}  // namespace

// The steady state — one tick per iteration — publishes the bare accumulator fraction, bit for bit.
// Asserted with EXPECT_EQ rather than EXPECT_FLOAT_EQ on purpose: at a span of one the mapping is an
// exact identity, not an approximation, and the suite holds it to that.
TEST(RunLoop, SingleTickFramesPublishRawAlpha) {
    SpanHarness h;
    h.step(msec(10.0));  // 0 ticks — parks a fraction in the accumulator
    EXPECT_EQ(h.lastTicks, 0);
    EXPECT_EQ(h.alpha, h.expectedRaw());

    for (int i = 0; i < 4; ++i) {
        h.step(msec(16.9));  // one tick each, leaving a moving remainder
        EXPECT_EQ(h.lastTicks, 1);
        EXPECT_EQ(h.alpha, h.expectedRaw());
    }
}

// An iteration that commits two ticks leaves the mirror spanning two fixed steps, so the fraction is
// mapped across the pair rather than reported as though it described one step.
TEST(RunLoop, MultiTickFrameMapsAlphaAcrossItsSpan) {
    SpanHarness h;
    h.step(msec(16.60));  // 0 ticks — parks the accumulator just under a period
    ASSERT_EQ(h.lastTicks, 0);

    h.step(msec(16.93));  // crosses two periods
    ASSERT_EQ(h.lastTicks, 2);
    EXPECT_FLOAT_EQ(h.alpha, (1.0f + h.expectedRaw()) / 2.0f);
    EXPECT_GT(h.alpha, 0.5f);   // over halfway along the doubled interval, not at its start
    EXPECT_LT(h.alpha, 1.0f);
}

// An iteration that commits nothing is still easing across the last commit's interval, so it keeps
// that span; the next single-tick commit returns the factor to the bare fraction.
TEST(RunLoop, ZeroTickFrameKeepsThePreviousSpan) {
    SpanHarness h;
    h.step(msec(16.60));
    h.step(msec(16.93));
    ASSERT_EQ(h.lastTicks, 2);

    h.step(msec(16.43));  // commits nothing — still spanning the pair
    ASSERT_EQ(h.lastTicks, 0);
    EXPECT_FLOAT_EQ(h.alpha, (1.0f + h.expectedRaw()) / 2.0f);

    h.step(msec(16.66));  // one tick — the span narrows back to a single step
    ASSERT_EQ(h.lastTicks, 1);
    EXPECT_EQ(h.alpha, h.expectedRaw());
}

// A frame long enough to hit the spiral clamp commits many ticks at once. The factor stays a valid
// blend factor and lands near the current state, which is where a large catch-up belongs.
TEST(RunLoop, AlphaStaysInUnitRangeAcrossASpiralClamp) {
    SpanHarness h;
    h.step(std::chrono::milliseconds{500});  // clamped to kMaxFrameTime
    EXPECT_GT(h.lastTicks, 10);              // a long catch-up, not one step
    EXPECT_GE(h.alpha, 0.0f);
    EXPECT_LT(h.alpha, 1.0f);
    // Within a fraction of one step of cur: across a span this long each step is a small part of the
    // whole interval, so the factor sits very near its top.
    EXPECT_GT(h.alpha, 0.99f);
}

// End to end: drive the loop through a two-tick commit and the starved iteration after it, mirroring
// the renderer's interpolator gating, and require the rendered position to keep tracking wall-clock.
// An object moving a constant distance per tick covers ground in proportion to elapsed time no matter
// how the ticks bunched — that is what interpolation is for, and this pair is where it is hardest.
TEST(RunLoop, InterpolatedPositionIsSmoothAcrossATwoTickPair) {
    constexpr int kPxPerTick = 10;

    ManualClock  clock;
    RunLoop      loop{clock};
    Interpolator interp;

    int simX = 0;
    loop.simTick([&](const InputState&) { simX += kPxPerTick; });

    double lastRendered = 0.0;
    double lastDelta    = 0.0;
    bool   haveHistory  = false;
    loop.renderLoop([&] {
        const FrameTiming t = frameTiming();

        FrameDrawState submission;
        submission.layers.push_back(DrawLayer{.key = "bg", .z = 0, .scroll = {simX, 0}});

        if (t.tickAdvanced) interp.reconcile(submission, t);  // the renderer's gating

        const auto pos = interp.interpolatedLayerScroll("bg", t);
        const double rendered = pos ? static_cast<double>(pos->x) : static_cast<double>(simX);
        lastDelta = haveHistory ? rendered - lastRendered : 0.0;
        lastRendered = rendered;
        haveHistory = true;
    });

    std::chrono::nanoseconds now{};
    const auto step = [&](std::chrono::nanoseconds dt) {
        now += dt;
        clock.set(now);
        loop.advance();
    };

    clock.set(now);
    loop.advance();  // baseline

    for (int i = 0; i < 4; ++i) step(kTickPeriod);  // mount the key and build prev/cur history

    // Distance the object should cover in a frame of `dt`, at a constant kPxPerTick per tick period.
    const auto expectedFor = [](std::chrono::nanoseconds dt) {
        return kPxPerTick * static_cast<double>(dt.count())
             / static_cast<double>(kTickPeriod.count());
    };
    constexpr double kTol = 0.25;  // px

    step(msec(16.60));  // 0 ticks
    EXPECT_NEAR(lastDelta, expectedFor(msec(16.60)), kTol);

    step(msec(16.93));  // 2 ticks — the simulation moves two steps in one frame
    EXPECT_NEAR(lastDelta, expectedFor(msec(16.93)), kTol);

    step(msec(16.43));  // 0 ticks — the starved frame after the pair
    EXPECT_NEAR(lastDelta, expectedFor(msec(16.43)), kTol);

    step(msec(16.66));  // 1 tick — back to the steady state
    EXPECT_NEAR(lastDelta, expectedFor(msec(16.66)), kTol);
}
