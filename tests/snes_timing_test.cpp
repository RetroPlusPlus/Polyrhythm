// The SNES timing presets: what a game's run loop ticks at when it keeps a SNES cadence, and the
// cycle arithmetic a clock that is not a whole number of hertz needs. The 60 Hz console's master
// clock is 236'250'000 / 11 Hz, so its CPU block carries the divisor and a draw divides by it. With
// the divisor at 1 a draw is the number it is without one.
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "manual_clock.h"
#include "retropp/input.h"
#include "retropp/run_loop.h"
#include "retropp/timing.h"

namespace retropp {
namespace {

using namespace std::chrono_literals;

// Case 6/7's constants at namespace scope — a lambda reading a block-scope constexpr is not portable
// across the five toolchains. Both terms of exactAfter fit 64 bits where the whole run's product does
// not.
constexpr std::int64_t  kHz60Ns  = 16'666'667;
constexpr std::uint64_t kScale   = 11ull * 1'000'000'000ull;
constexpr std::uint64_t kPerTick = 236'250'000ull * 16'666'667ull;
constexpr std::uint64_t kWhole   = kPerTick / kScale;   // 357'954
constexpr std::uint64_t kLeft    = kPerTick % kScale;   // 6'078'750'000

constexpr std::uint64_t exactAfter(std::uint64_t t) {
    return t * kWhole + t * kLeft / kScale;
}

TEST(SnesTiming, ThePresetPeriodsAreOneFrameOfEachRegion) {
    EXPECT_EQ(static_cast<std::int64_t>(TickPeriodNs::Snes), 16'639'263);
    EXPECT_EQ(static_cast<std::int64_t>(TickPeriodNs::SnesPal), 19'997'208);
    EXPECT_EQ(TimingProfile::Snes.tickPeriod(), 16'639'263ns);
    EXPECT_EQ(TimingProfile::SnesPal.tickPeriod(), 19'997'208ns);
}

TEST(SnesTiming, ThePresetsCarryTheMasterClockAsARatio) {
    ASSERT_TRUE(TimingProfile::Snes.cpu.has_value());
    EXPECT_EQ(TimingProfile::Snes.cpu->cpuClockHz, 236'250'000u);
    EXPECT_EQ(TimingProfile::Snes.cpu->cpuClockHzDivisor, 11u);
    EXPECT_EQ(TimingProfile::Snes.cpu->cyclesPerFrame, 357'366u);
    EXPECT_EQ(TimingProfile::Snes.cpu->doubleSpeedCyclesPerFrame, 357'366u);

    EXPECT_EQ(TimingProfile::SnesPal.cpu->cpuClockHz, 21'281'370u);
    EXPECT_EQ(TimingProfile::SnesPal.cpu->cpuClockHzDivisor, 1u);
    EXPECT_EQ(TimingProfile::SnesPal.cpu->cyclesPerFrame, 425'568u);
    EXPECT_EQ(TimingProfile::SnesPal.cpu->doubleSpeedCyclesPerFrame, 425'568u);
}

TEST(SnesTiming, ThePresetsAreUsableInConstantExpressions) {
    static_assert(TimingProfile::Snes.tickPeriodNs == TickPeriodNs::Snes);
    static_assert(TimingProfile::Snes.cpu->cpuClockHzDivisor == 11);
    static_assert(TimingProfile::SnesPal.cpuCyclesPerTick() == 425'568u);
    SUCCEED();
}

TEST(SnesTiming, TheStoredFrameBudgetWinsAtThePresetsOwnCadence) {
    const CycleDraw ntsc =
        TimingProfile::Snes.cyclesForTick(TimingProfile::Snes.tickPeriod(), 4'321u);
    EXPECT_EQ(ntsc.cycles, 357'366u);
    EXPECT_EQ(ntsc.carryNs, 4'321u);

    const CycleDraw pal =
        TimingProfile::SnesPal.cyclesForTick(TimingProfile::SnesPal.tickPeriod(), 4'321u);
    EXPECT_EQ(pal.cycles, 425'568u);
    EXPECT_EQ(pal.carryNs, 4'321u);

    // The derivation the stored count exists to beat: at the profile's own period the rate arm loses
    // a cycle to the rounded ns period.
    EXPECT_EQ(TimingProfile::Snes.cpu->cyclesFor(16'639'263ns).cycles, 357'365u);
    EXPECT_EQ(TimingProfile::SnesPal.cpu->cyclesFor(19'997'208ns).cycles, 425'567u);
}

TEST(SnesTiming, OneDrawAtAForeignCadenceDividesByTheClocksDivisor) {
    const CycleDraw a = TimingProfile::Snes.cpu->cyclesFor(16'666'667ns);
    EXPECT_EQ(a.cycles, 357'954u);
    EXPECT_EQ(a.carryNs, 6'078'750'000u);
    EXPECT_LT(a.carryNs, 11'000'000'000u);

    const CycleDraw b = TimingProfile::Snes.cpu->cyclesFor(16'742'706ns);
    EXPECT_EQ(b.cycles, 359'587u);
    EXPECT_EQ(b.carryNs, 7'292'500'000u);

    const CycleDraw pal = TimingProfile::SnesPal.cpu->cyclesFor(16'666'667ns);
    EXPECT_EQ(pal.cycles, 354'689u);
    EXPECT_EQ(pal.carryNs, 507'093'790u);

    // The foreign arm is the rate arm.
    const CycleDraw tick = TimingProfile::Snes.cyclesForTick(16'666'667ns);
    EXPECT_EQ(tick.cycles, a.cycles);
    EXPECT_EQ(tick.carryNs, a.carryNs);
}

TEST(SnesTiming, AMillionForeignTicksNeverLoseACycle) {
    std::uint64_t total = 0;
    CycleDraw draw{};
    for (std::uint64_t t = 1; t <= 1'000'000u; ++t) {
        draw = TimingProfile::Snes.cpu->cyclesFor(std::chrono::nanoseconds{kHz60Ns}, draw.carryNs);
        total += draw.cycles;
        ASSERT_EQ(total, exactAfter(t)) << "tick " << t;
    }
    EXPECT_EQ(total, 357'954'552'613u);
    EXPECT_EQ(draw.carryNs, 7'000'000'000u);
}

TEST(SnesTiming, DroppingTheCarryFallsBehind) {
    std::uint64_t total = 0;
    for (std::uint64_t t = 1; t <= 1'000'000u; ++t) {
        total += TimingProfile::Snes.cpu->cyclesFor(std::chrono::nanoseconds{kHz60Ns}, 0u).cycles;
    }
    EXPECT_EQ(total, 357'954'000'000u);
    EXPECT_EQ(357'954'552'613u - total, 552'613u);
}

TEST(SnesTiming, AGameBoyDrawIsTheNumberItWasBeforeTheDivisor) {
    EXPECT_EQ(TimingProfile::GameBoyColor.cpu->cpuClockHzDivisor, 1u);
    EXPECT_EQ(TimingProfile::GameBoy.cpu->cpuClockHzDivisor, 1u);

    EXPECT_TRUE((CpuTiming{.cpuClockHz = 4'194'304, .cyclesPerFrame = 70'224,
                           .doubleSpeedCyclesPerFrame = 140'448}) == *TimingProfile::GameBoyColor.cpu);

    struct Row {
        std::int64_t  spanNs;
        std::uint64_t carryIn;
        std::uint64_t cycles;
        std::uint64_t carryOut;
    };
    constexpr Row rows[] = {
        {.spanNs = 16'742'706, .carryIn = 0,           .cycles = 70'223,  .carryOut = 998'746'624},
        {.spanNs = 16'742'706, .carryIn = 999'999'999, .cycles = 70'224,  .carryOut = 998'746'623},
        {.spanNs = 16'742'706, .carryIn = 123'456'789, .cycles = 70'224,  .carryOut = 122'203'413},
        {.spanNs = 16'666'667, .carryIn = 0,           .cycles = 69'905,  .carryOut = 68'064'768},
        {.spanNs = 16'666'667, .carryIn = 999'999'999, .cycles = 69'906,  .carryOut = 68'064'767},
        {.spanNs = 16'666'667, .carryIn = 123'456'789, .cycles = 69'905,  .carryOut = 191'521'557},
        {.spanNs = 4'000'000,  .carryIn = 0,           .cycles = 16'777,  .carryOut = 216'000'000},
        {.spanNs = 4'000'000,  .carryIn = 999'999'999, .cycles = 16'778,  .carryOut = 215'999'999},
        {.spanNs = 4'000'000,  .carryIn = 123'456'789, .cycles = 16'777,  .carryOut = 339'456'789},
        {.spanNs = 1,          .carryIn = 0,           .cycles = 0,       .carryOut = 4'194'304},
        {.spanNs = 1,          .carryIn = 999'999'999, .cycles = 1,       .carryOut = 4'194'303},
        {.spanNs = 1,          .carryIn = 123'456'789, .cycles = 0,       .carryOut = 127'651'093},
        {.spanNs = 33'333'333, .carryIn = 0,           .cycles = 139'810, .carryOut = 131'935'232},
        {.spanNs = 33'333'333, .carryIn = 999'999'999, .cycles = 139'811, .carryOut = 131'935'231},
        {.spanNs = 33'333'333, .carryIn = 123'456'789, .cycles = 139'810, .carryOut = 255'392'021},
    };
    for (const Row& r : rows) {
        const CycleDraw d =
            TimingProfile::GameBoyColor.cpu->cyclesFor(std::chrono::nanoseconds{r.spanNs}, r.carryIn);
        EXPECT_EQ(d.cycles, r.cycles) << "span " << r.spanNs << " carryIn " << r.carryIn;
        EXPECT_EQ(d.carryNs, r.carryOut) << "span " << r.spanNs << " carryIn " << r.carryIn;
    }
}

TEST(SnesTiming, AZeroDivisorNamesNoClockAndDrawsNothing) {
    constexpr CpuTiming noClock{.cpuClockHz                = 236'250'000,
                                .cyclesPerFrame            = 357'366,
                                .doubleSpeedCyclesPerFrame = 357'366,
                                .cpuClockHzDivisor         = 0};
    const CycleDraw d = noClock.cyclesFor(16'666'667ns, 12'345u);
    EXPECT_EQ(d.cycles, 0u);
    EXPECT_EQ(d.carryNs, 12'345u);
}

TEST(SnesTiming, ARunLoopTicksAtThePresetsPeriod) {
    test::ManualClock clock;
    RunLoop loop{clock, TimingProfile::Snes};
    EXPECT_EQ(loop.tickPeriod(), 16'639'263ns);

    int ticks = 0;
    loop.simTick([&](const InputState&) { ++ticks; });
    loop.advance();                              // settle baseline
    clock.advanceBy(loop.tickPeriod() * 3);
    loop.advance();
    EXPECT_EQ(ticks, 3);
    EXPECT_EQ(loop.tickCount(), 3u);
}

TEST(SnesTiming, TicksForDurationCountsEachRegionsFrames) {
    EXPECT_EQ(TimingProfile::Snes.ticksForDuration(1s), 60u);
    EXPECT_EQ(TimingProfile::Snes.ticksForDuration(10s), 601u);
    EXPECT_EQ(TimingProfile::SnesPal.ticksForDuration(1s), 50u);
    EXPECT_EQ(TimingProfile::SnesPal.ticksForDuration(10s), 500u);
}

}  // namespace
}  // namespace retropp
