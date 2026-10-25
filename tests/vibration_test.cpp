// Controller vibration: the pure sampleVibration resolver, the game-owned VibrationPlayer, and the
// platform flush + per-slot diff reconciliation. Entirely device-free — the pattern grammar is plain
// data, the resolver is a pure function of (frames, elapsed ticks, TimingProfile, PlaybackMode), and the
// flush path is exercised through MockPlatform (records the flushed MotorLevels) driven by the windowed
// host over a ManualClock. Tick numbers are pinned to the GameBoyColor cadence (100ms == 6 ticks) so a
// cadence regression is loud, exactly as animation_test does.
//
// The load-bearing semantic pinned here is the finished→silence rule: past a finite mode's end
// sampleVibration returns { levels = {}, finished = true } — NOT the clamped last frame that sampleAnimation
// holds — because endless rumble reads as a broken game. The rule lives in the resolver; the player is a
// thin cache over it.

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "retropp/animation.h"      // PlaybackMode
#include "retropp/run_loop.h"
#include "retropp/timing.h"
#include "retropp/vibration.h"
#include "retropp/windowed_host.h"

#include "manual_clock.h"
#include "mock_platform.h"

using namespace retropp;
using namespace std::chrono_literals;
using test::ManualClock;
using test::MockPlatform;

namespace {

const TimingProfile gbc = TimingProfile::GameBoyColor;
constexpr auto      kTickPeriod = TimingProfile::GameBoyColor.tickPeriod();

// 3 frames, 100ms each (6 ticks each → 18 total), distinct motor levels + labels.
VibrationPattern makeUniform() {
    return VibrationPattern{{
        VibrationFrame{"a", MotorLevels{.low = 10}, 100ms},
        VibrationFrame{"b", MotorLevels{.low = 20}, 100ms},
        VibrationFrame{"c", MotorLevels{.low = 30}, 100ms},
    }};
}

MotorLevels lv(const VibrationPattern& p, std::uint64_t t, PlaybackMode m) {
    return sampleVibration(p, t, gbc, m).levels;
}
bool fin(const VibrationPattern& p, std::uint64_t t, PlaybackMode m) {
    return sampleVibration(p, t, gbc, m).finished;
}

}  // namespace

// ── MotorLevels ─────────────────────────────────────────────────────────────────────────────────────

TEST(MotorLevels, DefaultsToSilenceAndComparesByValue) {
    EXPECT_EQ(MotorLevels{}, (MotorLevels{.low = 0, .high = 0, .triggerLeft = 0, .triggerRight = 0}));
    EXPECT_NE(MotorLevels{}, (MotorLevels{.low = 1}));
    EXPECT_EQ((MotorLevels{.high = 200}), (MotorLevels{.high = 200}));
    // Every motor is optional — a partial buzz names only what it drives.
    const MotorLevels m{.low = 200, .high = 60};
    EXPECT_EQ(m.low, 200);
    EXPECT_EQ(m.high, 60);
    EXPECT_EQ(m.triggerLeft, 0);
    EXPECT_EQ(m.triggerRight, 0);
}

// ── The pinned bridge: 100ms == 6 ticks, total == 18 ─────────────────────────────────────────────────

TEST(Vibration, FrameTicksAndTotalArePinnedToGbcCadence) {
    EXPECT_EQ(gbc.ticksForDuration(100ms), 6u);
    EXPECT_EQ(totalTicks(makeUniform(), gbc), 18u);
}

// ── sampleVibration — LoopIndefinitely ───────────────────────────────────────────────────────────────

TEST(SampleVibrationLoopIndefinitely, FrameWindowsAndWrap) {
    const VibrationPattern p = makeUniform();
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(lv(p, 0, loop), (MotorLevels{.low = 10}));    // window 0 = [0,6)
    EXPECT_EQ(lv(p, 5, loop), (MotorLevels{.low = 10}));
    EXPECT_EQ(lv(p, 6, loop), (MotorLevels{.low = 20}));    // window 1 = [6,12)
    EXPECT_EQ(lv(p, 12, loop), (MotorLevels{.low = 30}));   // window 2 = [12,18)
    EXPECT_EQ(lv(p, 18, loop), (MotorLevels{.low = 10}));   // wraps to frame 0
    EXPECT_FALSE(fin(p, 0, loop));
    EXPECT_FALSE(fin(p, 100000, loop));  // never finished
}

// ── sampleVibration — Single (the finished→silence pin) ──────────────────────────────────────────────

TEST(SampleVibrationSingle, PlaysThenReturnsSilenceNotTheLastFrame) {
    const VibrationPattern p = makeUniform();
    const auto once = PlaybackMode::single();
    EXPECT_EQ(lv(p, 0, once), (MotorLevels{.low = 10}));
    EXPECT_EQ(lv(p, 17, once), (MotorLevels{.low = 30}));
    EXPECT_FALSE(fin(p, 17, once));
    // At/after total: SILENCE + finished — diverges from sampleAnimation (which clamps to the last frame).
    EXPECT_EQ(lv(p, 18, once), MotorLevels{});
    EXPECT_TRUE(fin(p, 18, once));
    EXPECT_EQ(lv(p, 999, once), MotorLevels{});
    EXPECT_TRUE(fin(p, 999, once));
}

// ── sampleVibration — LoopNTimes / PlayForDuration ───────────────────────────────────────────────────

TEST(SampleVibrationLoopNTimes, WrapsForNPassesThenSilence) {
    const VibrationPattern p = makeUniform();  // total 18
    const auto n3 = PlaybackMode::loopNTimes(3);  // ends at 54
    EXPECT_EQ(lv(p, 6, n3), (MotorLevels{.low = 20}));
    EXPECT_EQ(lv(p, 24, n3), (MotorLevels{.low = 20}));  // pass 2, window 1
    EXPECT_FALSE(fin(p, 53, n3));
    EXPECT_EQ(lv(p, 54, n3), MotorLevels{});  // finished → silence
    EXPECT_TRUE(fin(p, 54, n3));
    EXPECT_TRUE(fin(p, 0, PlaybackMode::loopNTimes(0)));  // zero passes → immediately finished
}

TEST(SampleVibrationPlayForDuration, WrapsUntilCutoffThenSilence) {
    const VibrationPattern p = makeUniform();  // total 18
    const std::uint64_t cut = gbc.ticksForDuration(500ms);
    ASSERT_EQ(cut, 30u);
    const auto dur = PlaybackMode::playForDuration(500ms);
    EXPECT_EQ(lv(p, 24, dur), (MotorLevels{.low = 20}));  // 24 % 18 = 6 → window 1, still playing
    EXPECT_FALSE(fin(p, 29, dur));
    EXPECT_EQ(lv(p, 30, dur), MotorLevels{});  // finished → silence
    EXPECT_TRUE(fin(p, 30, dur));
}

// ── Degenerate / zero-tick guards ────────────────────────────────────────────────────────────────────

TEST(SampleVibrationDegenerate, EmptyPatternIsFinishedSilence) {
    VibrationPattern empty{};
    EXPECT_EQ(totalTicks(empty, gbc), 0u);
    const VibrationState s = sampleVibration(empty, 0, gbc, PlaybackMode::loopIndefinitely());
    EXPECT_EQ(s.levels, MotorLevels{});
    EXPECT_TRUE(s.finished);
}

TEST(SampleVibrationDegenerate, ZeroDurationFrameIsSkipped) {
    // 8ms → 0 ticks. The middle frame is instantaneous: it must never be the resting levels or stall.
    ASSERT_EQ(gbc.ticksForDuration(8ms), 0u);
    VibrationPattern p{{
        VibrationFrame{"x", MotorLevels{.low = 10}, 100ms},   // 6
        VibrationFrame{"gone", MotorLevels{.low = 99}, 8ms},  // 0 — skipped, never seen
        VibrationFrame{"y", MotorLevels{.low = 20}, 100ms},   // 6
    }};
    EXPECT_EQ(totalTicks(p, gbc), 12u);
    const auto loop = PlaybackMode::loopIndefinitely();
    EXPECT_EQ(lv(p, 0, loop), (MotorLevels{.low = 10}));
    EXPECT_EQ(lv(p, 6, loop), (MotorLevels{.low = 20}));   // straight to frame 2 — never the 0-tick frame
    EXPECT_EQ(lv(p, 12, loop), (MotorLevels{.low = 10}));  // wraps, never resting on the skipped frame
}

// ── Programmatic symbolic access ─────────────────────────────────────────────────────────────────────

TEST(VibrationPatternAccess, IndexOfAndFind) {
    const VibrationPattern p = makeUniform();
    ASSERT_TRUE(p.indexOf("b").has_value());
    EXPECT_EQ(*p.indexOf("b"), 1u);
    EXPECT_EQ(p.find("c"), &p.frames[2]);
    EXPECT_EQ(p.indexOf("missing"), std::nullopt);
    EXPECT_EQ(p.find("missing"), nullptr);
}

// ── VibrationPlayer ──────────────────────────────────────────────────────────────────────────────────

TEST(VibrationPlayer, BareAdvanceLoopsAndLevelsMatchTheResolver) {
    const VibrationPattern p = makeUniform();
    VibrationPlayer player{.pattern = &p};  // default profile = GBC
    player.advance(PlaybackMode::loopIndefinitely(), 6);  // tick 6 → window 1
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 20}));
    EXPECT_FALSE(player.finished());
    player.advance(PlaybackMode::loopIndefinitely(), 12);  // tick 18 → wraps to window 0
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 10}));
}

TEST(VibrationPlayer, FinishedNonLoopingReturnsSilenceSoFeedingItAutoStops) {
    const VibrationPattern p = makeUniform();
    VibrationPlayer player{.pattern = &p};
    player.advance(PlaybackMode::single(), 18);  // exactly past the end
    EXPECT_TRUE(player.finished());
    EXPECT_EQ(player.levels(), MotorLevels{});  // silence, not the last frame — passing it to vibration() stops
}

TEST(VibrationPlayer, AccruesOnlyWhilePlayingAndPauseFreezes) {
    const VibrationPattern p = makeUniform();
    VibrationPlayer player{.pattern = &p};
    player.advance(PlaybackMode::loopIndefinitely(), 6);  // tick 6 → window 1
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 20}));
    player.pause();
    player.advance(PlaybackMode::loopIndefinitely(), 100);  // frozen — levels + elapsed unchanged
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 20}));
    EXPECT_EQ(player.elapsedTicks, 6u);
    player.play();
    player.advance(PlaybackMode::loopIndefinitely(), 6);  // tick 12 → window 2
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 30}));
}

TEST(VibrationPlayer, StopRewindsToStartAndRestartPlays) {
    const VibrationPattern p = makeUniform();
    VibrationPlayer player{.pattern = &p};
    player.advance(PlaybackMode::loopIndefinitely(), 12);  // window 2
    player.stop();
    EXPECT_EQ(player.elapsedTicks, 0u);
    EXPECT_FALSE(player.playing);
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 10}));  // rewound to the first frame
    player.restart();
    EXPECT_EQ(player.elapsedTicks, 0u);
    EXPECT_TRUE(player.playing);
    EXPECT_FALSE(player.finished());
}

TEST(VibrationPlayer, SeekByIndexAndLabel) {
    const VibrationPattern p = makeUniform();
    VibrationPlayer player{.pattern = &p};
    player.seek(2);
    EXPECT_EQ(player.elapsedTicks, 12u);  // window 2 start
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 30}));
    player.seek("a");
    EXPECT_EQ(player.elapsedTicks, 0u);
    EXPECT_EQ(player.levels(), (MotorLevels{.low = 10}));
    player.seek("nope");  // absent → no-op
    EXPECT_EQ(player.elapsedTicks, 0u);
}

TEST(VibrationPlayer, NullPatternIsSilent) {
    VibrationPlayer player{};  // no pattern
    player.advance();
    EXPECT_EQ(player.levels(), MotorLevels{});
}

// ── Platform flush + per-slot diff reconciliation (host-driven, device-free) ─────────────────────────
//
// The host runs one flush per frame that committed ≥ 1 tick. Each case pre-consumes the run loop's
// lazy baseline with a manual advance() so the host's first iteration already produces a tick, then a
// per-pump hook advances the clock a full tick period so exactly one tick fires per iteration. The tick
// callback declares vibration; MockPlatform records the flushed (post-diff) MotorLevels.

TEST(VibrationFlush, ConstantDeclarationFlushesOnceThenDiffsAway) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.advance();  // consume the lazy baseline so iteration 1 already ticks
    MockPlatform platform{3};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
    loop.simTick([&](const InputState&) {
        platform.gamepad(0).vibration({.low = 200, .high = 60});  // the SAME value every tick
    });

    WindowedHost{loop, platform}.run();

    ASSERT_EQ(platform.flushedVibrations().size(), 1u);  // 3 ticks, 3 identical declares → ONE flush
    EXPECT_EQ(platform.flushedVibrations()[0].player, 0);
    EXPECT_EQ(platform.flushedVibrations()[0].levels, (MotorLevels{.low = 200, .high = 60}));
}

TEST(VibrationFlush, ChangedDeclarationEmitsExactlyOneReplace) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.advance();
    MockPlatform platform{2};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
    int t = 0;
    loop.simTick([&](const InputState&) {
        ++t;
        platform.gamepad(0).vibration({.low = static_cast<std::uint8_t>(t == 1 ? 200 : 100)});
    });

    WindowedHost{loop, platform}.run();

    ASSERT_EQ(platform.flushedVibrations().size(), 2u);
    EXPECT_EQ(platform.flushedVibrations()[0].levels, (MotorLevels{.low = 200}));
    EXPECT_EQ(platform.flushedVibrations()[1].levels, (MotorLevels{.low = 100}));
}

TEST(VibrationFlush, DeclaredSilenceAndNoCallBothStop) {
    // A tick that declares {} and a tick that makes NO call both reconcile to a single stop flush.
    for (const bool explicitSilence : {true, false}) {
        ManualClock clock;
        RunLoop     loop{clock};
        loop.advance();
        MockPlatform platform{2};
        platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
        int t = 0;
        loop.simTick([&](const InputState&) {
            ++t;
            if (t == 1) {
                platform.gamepad(0).vibration({.low = 200});
            } else if (explicitSilence) {
                platform.gamepad(0).vibration({});  // explicit silence
            }
            // else (t == 2, !explicitSilence): no call at all — a silent tick
        });

        WindowedHost{loop, platform}.run();

        ASSERT_EQ(platform.flushedVibrations().size(), 2u);
        EXPECT_EQ(platform.flushedVibrations()[0].levels, (MotorLevels{.low = 200}));
        EXPECT_EQ(platform.flushedVibrations()[1].levels, MotorLevels{});  // stop
    }
}

TEST(VibrationFlush, ZeroTickHostFrameDoesNotResetAHeldRumble) {
    // Iteration 2 runs ZERO ticks (its pump does not advance the clock): the held rumble must survive —
    // no stop flush — because the game had no tick to re-declare in. Only the initial flush is recorded.
    ManualClock clock;
    RunLoop     loop{clock};
    loop.advance();
    MockPlatform platform{3};
    int pump = 0;
    platform.setOnPump([&] {
        ++pump;
        if (pump != 2) clock.advanceBy(kTickPeriod);  // iteration 2 produces no tick
    });
    loop.simTick([&](const InputState&) {
        platform.gamepad(0).vibration({.low = 200});  // a constant held rumble
    });

    WindowedHost{loop, platform}.run();

    // Iteration 1 flushes {200}; iteration 2 (zero-tick) does NOT flush (would have stopped it);
    // iteration 3 re-declares {200} → diffs away. Exactly one flush total.
    ASSERT_EQ(platform.flushedVibrations().size(), 1u);
    EXPECT_EQ(platform.flushedVibrations()[0].levels, (MotorLevels{.low = 200}));
}

TEST(VibrationFlush, SlotWithNoPadRecordsNothing) {
    ManualClock clock;
    RunLoop     loop{clock};
    loop.advance();
    MockPlatform platform{2};
    platform.setOnPump([&] { clock.advanceBy(kTickPeriod); });
    platform.setGamepadPresent(0, false);  // no pad on slot 0
    loop.simTick([&](const InputState&) { platform.gamepad(0).vibration({.low = 200}); });

    WindowedHost{loop, platform}.run();

    EXPECT_TRUE(platform.flushedVibrations().empty());  // the handle no-ops; nothing reaches a device
}

TEST(VibrationSeam, GamepadIndexClampsAndKeyboardSlotNoOps) {
    // The output handle is on the base seam: an out-of-range slot clamps rather than reading out of
    // bounds, and declaring for a slot with no pad simply records nothing (no crash).
    MockPlatform platform{1};
    Platform&    seam = platform;
    platform.setGamepadPresent(3, false);
    seam.gamepad(99).vibration({.low = 50});  // clamps into [0, kMaxPlayers) → slot 3 (marked absent)
    seam.flushVibration();
    EXPECT_TRUE(platform.flushedVibrations().empty());
}

// The SDL shutdown-zero (the destructor issues SDL_Rumble*(0,0,0) so a held long-duration rumble never
// outlives the loop) is a device-side behavior on SdlPlatform — it talks straight to SDL, not through
// the base emit hook — so it is dev-verified on real hardware (the input_probe vibration mode), not
// here. MockPlatform models no device teardown.
