// How many of a machine's cycles one engine tick is worth, when the tick is not that machine's own
// frame.
//
// A machine running at its native cadence has an exact stored answer — a Game Boy frame IS 70'224
// cycles — and nothing here disturbs it. The case these cases exist for is a machine hosted at a
// cadence that is not its own, where there is no frame count to reach for and the clock rate is the
// only thing that answers. A rate that does not divide the tick period leaves a fraction of a cycle
// behind every tick, and the whole point is that the fraction is kept and spent later rather than
// dropped: the running total stays exact over any number of ticks, and the error at any instant is
// under one cycle.
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "retropp/gb.h"
#include "retropp/timing.h"
#include "retropp/vm.h"

namespace retropp {
namespace {

using namespace std::chrono_literals;

// A machine whose clock divides no round tick period — the case the carry exists for. The rate is a
// plausible 16-bit-era CPU clock; what matters is that it is coprime with the periods below.
constexpr CpuTiming kForeignMachine{
    .cpuClockHz = 3'579'545, .cyclesPerFrame = 59'561, .doubleSpeedCyclesPerFrame = 119'122};

// The exact cycle count for `ticks` of `periodNs` at `clockHz`, in integer arithmetic — the value the
// carried draw must agree with at every step.
constexpr std::uint64_t exactCycles(std::uint64_t clockHz, std::int64_t periodNs,
                                    std::uint64_t ticks) {
    return clockHz * static_cast<std::uint64_t>(periodNs) * ticks / 1'000'000'000u;
}

TEST(CycleBudget, TheNativeFrameCountIsUntouched) {
    // The stored count is the exact hardware fact; deriving it from the rounded ns period would lose
    // a cycle a frame (4'194'304 x 16'742'706 = 70'223.998...), which is why the native path stores it.
    EXPECT_EQ(TimingProfile::GameBoyColor.cpuCyclesPerTick(), 70'224u);
    EXPECT_EQ(TimingProfile::GameBoy.cpuCyclesPerTick(), 70'224u);
}

TEST(CycleBudget, OneDrawTruncatesAndKeepsTheRemainder) {
    const CycleDraw draw = kForeignMachine.cyclesFor(16'742'706ns);
    EXPECT_EQ(draw.cycles, 59'931u);          // floor(3'579'545 x 16'742'706 / 1e9)
    EXPECT_EQ(draw.carryNs, 269'548'770u);    // the fraction, kept rather than dropped
    EXPECT_LT(draw.carryNs, 1'000'000'000u);
}

TEST(CycleBudget, AZeroSpanDrawsNothingAndPreservesTheCarry) {
    const CycleDraw draw = kForeignMachine.cyclesFor(0ns, 12'345u);
    EXPECT_EQ(draw.cycles, 0u);
    EXPECT_EQ(draw.carryNs, 12'345u);
}

TEST(CycleBudget, ANegativeSpanDrawsNothingAndPreservesTheCarry) {
    const CycleDraw draw = kForeignMachine.cyclesFor(-5ns, 12'345u);
    EXPECT_EQ(draw.cycles, 0u);
    EXPECT_EQ(draw.carryNs, 12'345u);
}

TEST(CycleBudget, AMachineWithNoCpuModelYieldsNoCyclesPerTick) {
    const TimingProfile bare{.tickPeriodNs = TickPeriodNs::Hz60, .cpu = {}};
    EXPECT_EQ(bare.cpuCyclesPerTick(), 0u);
}

// The property the whole mechanism exists for: over any number of ticks the carried total equals the
// exact rate x time, and never wanders more than a cycle from it along the way.
TEST(CycleBudget, TheCarriedTotalStaysExactOverManyTicks) {
    constexpr std::int64_t kPeriodNs = 16'742'706;  // a Game Boy cadence, hosting a foreign machine
    constexpr std::uint64_t kTicks = 10'000;

    std::uint64_t total = 0;
    CycleDraw     draw{};
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        draw = kForeignMachine.cyclesFor(std::chrono::nanoseconds{kPeriodNs}, draw.carryNs);
        total += draw.cycles;

        const std::uint64_t exact = exactCycles(kForeignMachine.cpuClockHz, kPeriodNs, t);
        ASSERT_GE(total + 1, exact) << "fell behind at tick " << t;
        ASSERT_LE(total, exact + 1) << "ran ahead at tick " << t;
    }
    EXPECT_EQ(total, exactCycles(kForeignMachine.cpuClockHz, kPeriodNs, kTicks));
}

// Dropping the carry is the defect this guards: the truncated remainder is lost every tick, so the
// total falls behind without bound instead of staying within a cycle.
TEST(CycleBudget, DroppingTheCarryWouldFallBehindWithoutBound) {
    constexpr std::int64_t kPeriodNs = 16'742'706;
    constexpr std::uint64_t kTicks = 10'000;

    std::uint64_t carried = 0;
    std::uint64_t dropped = 0;
    CycleDraw     draw{};
    for (std::uint64_t t = 0; t < kTicks; ++t) {
        draw = kForeignMachine.cyclesFor(std::chrono::nanoseconds{kPeriodNs}, draw.carryNs);
        carried += draw.cycles;
        dropped += kForeignMachine.cyclesFor(std::chrono::nanoseconds{kPeriodNs}).cycles;  // carry lost
    }
    EXPECT_EQ(carried, exactCycles(kForeignMachine.cpuClockHz, kPeriodNs, kTicks));
    EXPECT_EQ(carried - dropped, 2'695u);  // a whole frame's worth lost, and still growing
}

TEST(CycleBudget, TwoTicksComposeIntoTheSameTotalAsOneDoubleSpan) {
    constexpr std::chrono::nanoseconds kPeriod{16'742'706};

    const CycleDraw first  = kForeignMachine.cyclesFor(kPeriod);
    const CycleDraw second = kForeignMachine.cyclesFor(kPeriod, first.carryNs);
    const CycleDraw once   = kForeignMachine.cyclesFor(kPeriod * 2);

    EXPECT_EQ(first.cycles + second.cycles, once.cycles);
    EXPECT_EQ(second.carryNs, once.carryNs);
}

// A machine hosted at a cadence that is not its own draws a different budget than its own frame
// count — which is the entire reason the rate-derived path exists.
TEST(CycleBudget, AForeignCadenceDrawsADifferentBudgetThanTheMachinesOwnFrame) {
    const CycleDraw atGbCadence = kForeignMachine.cyclesFor(16'742'706ns);
    EXPECT_NE(atGbCadence.cycles, kForeignMachine.cyclesPerFrame);
}

// ── The VM verb ─────────────────────────────────────────────────────────────────────────────────

// rDIV is a SYNTHESIZED register: SameBoy answers a CPU read of 0xFF04 from its divider counter, and
// the raw IO storage a region read returns holds nothing useful. So the divider is observed the way
// the machine sees it — through a routine that reads it.
std::uint8_t readDivider(Vm& vm) {
    static constexpr std::array<std::uint8_t, 3> kReadDiv{0xF0, 0x04, 0xC9};  // ldh a,[0xFF04] ; ret
    auto routine = vm.uploadRoutine<std::uint8_t()>(std::span<const std::uint8_t>(kReadDiv),
                                                    RoutineBinding{.output = gb::A});
    return routine();
}

TEST(CycleBudget, AdvanceTickMovesTheMachineByItsOwnTickWorth) {
    // Each VM pays for exactly one routine call, so the divider difference is the ticks alone.
    Vm::GBC ticked;
    Vm::GBC still;
    for (int i = 0; i < 8; ++i) {
        ticked.advanceTick();
    }

    EXPECT_NE(readDivider(ticked), readDivider(still));
}

// advanceTick() spends what a tick is worth: over a long run the machine lands where spending the
// per-tick budget by hand lands it.
//
// This does NOT pin which arm cyclesForTick picked, and cannot. The carry recovers nearly everything
// the derivation would lose — 512 ticks of the derived arm run about 7 cycles behind the stored one,
// while rDIV moves once every 256 cycles — so the divider is far too coarse to tell them apart. The
// arm selection is pinned arithmetically by TheNativeCadenceSpellingsAgreeWhicheverWayItIsAsked.
TEST(CycleBudget, AdvanceTickIsTheSameAsSpendingOneTicksCyclesDirectly) {
    constexpr int kTicks = 512;

    Vm::GBC byTick;
    Vm::GBC byCycles;
    for (int i = 0; i < kTicks; ++i) {
        byTick.advanceTick();
        byCycles.advanceClock(TimingProfile::GameBoyColor.cpuCyclesPerTick());
    }

    EXPECT_EQ(readDivider(byTick), readDivider(byCycles));
}

TEST(CycleBudget, AdvanceTickAtAForeignCadenceSpendsThatCadencesWorth) {
    Vm::GBC atNative;
    Vm::GBC atShort;
    for (int i = 0; i < 16; ++i) {
        atNative.advanceTick();
        atShort.advanceTick(4ms);  // a much shorter tick than the machine's own frame
    }

    EXPECT_NE(readDivider(atNative), readDivider(atShort));
}

// A machine's tick is its own. A Game Boy handed a 60 Hz profile with no CPU block advances one
// Game Boy frame a tick — exactly what a Vm::GBC does.
TEST(CycleBudget, AdvanceTickSpendsTheMachinesOwnFrameUnderAnyProfile) {
    const TimingProfile hz60{.tickPeriodNs = TickPeriodNs::Hz60, .cpu = {}};
    Vm      ticked{VMPlatform::GameBoyColor, hz60};
    Vm      still{VMPlatform::GameBoyColor, hz60};
    Vm::GBC own;
    for (int i = 0; i < 8; ++i) {
        ticked.advanceTick();
        own.advanceTick();
    }

    const std::uint8_t tickedDivider = readDivider(ticked);
    EXPECT_EQ(tickedDivider, readDivider(own));
    EXPECT_NE(tickedDivider, readDivider(still));
}

TEST(CycleBudget, TheNativeCadenceSpellingsAgreeWhicheverWayItIsAsked) {
    const TimingProfile gbc = TimingProfile::GameBoyColor;
    // Naming the profile's own period explicitly must not select the derived arm — the stored frame
    // count is the exact answer either way.
    EXPECT_EQ(gbc.cyclesForTick(gbc.tickPeriod()).cycles, gbc.cpuCyclesPerTick());
    EXPECT_EQ(gbc.cyclesForTick(gbc.tickPeriod()).carryNs, 0u);
}

}  // namespace
}  // namespace retropp
