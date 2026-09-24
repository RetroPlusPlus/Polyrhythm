// A machine's clock is its core's own: the rate as the exact ratio it is, the cycles one of its
// frames takes, and the arithmetic that paces a machine against the wall clock from those alone.

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/vm/run_governor.h"
#include "src/vm/vm_backend.h"
#include "src/vm/vm_core_access.h"
#include "src/vm/vm_testing.h"
#include "src/vm/wide_math.h"
#include "tests/mock_vm_backend.h"

namespace retropp {
namespace {

using namespace std::chrono_literals;

using vm::MachineClock;
using vm::mulAddDiv;
using vm::RunGovernor;
using vm::VmTestAccess;
using Clock = std::chrono::steady_clock;

Clock::time_point at(std::int64_t ns) { return Clock::time_point{std::chrono::nanoseconds{ns}}; }

// ── The wide multiply-add-divide ────────────────────────────────────────────────────────────────

TEST(WideMath, AProductThatFitsSixtyFourBitsDividesAsItAlwaysDid) {
    const vm::WideQuotient a = mulAddDiv(4'194'304, 16'742'706, 0, 1'000'000'000);
    EXPECT_EQ(a.quotient, 70'223u);
    EXPECT_EQ(a.remainder, 998'746'624u);
    const vm::WideQuotient b = mulAddDiv(4'194'304, 16'742'706, 999'999'999, 1'000'000'000);
    EXPECT_EQ(b.quotient, 70'224u);
    EXPECT_EQ(b.remainder, 998'746'623u);
}

TEST(WideMath, AProductPastSixtyFourBitsIsCarriedWhole) {
    EXPECT_EQ(mulAddDiv(236'250'000ull * 1024, 250'000'000, 0, 11'000'000'000).quotient,
              5'498'181'818u);
    const vm::WideQuotient max = mulAddDiv(UINT64_MAX, UINT64_MAX, 0, UINT64_MAX);
    EXPECT_EQ(max.quotient, UINT64_MAX);
    EXPECT_EQ(max.remainder, 0u);
}

TEST(WideMath, IsUsableInConstantExpressions) {
    static_assert(mulAddDiv(7, 6, 3, 5).quotient == 9 && mulAddDiv(7, 6, 3, 5).remainder == 0);
}

// ── A machine's own clock ───────────────────────────────────────────────────────────────────────

constexpr vm::MachineClock kGameBoy{.hertzNumerator = 4'194'304, .hertzDivisor = 1,
                                    .cyclesPerFrame = 70'224};
constexpr vm::MachineClock kNtsc{.hertzNumerator = 236'250'000, .hertzDivisor = 11,
                                 .cyclesPerFrame = 357'366};
constexpr vm::MachineClock kPal{.hertzNumerator = 21'281'370, .hertzDivisor = 1,
                                .cyclesPerFrame = 425'568};

TEST(MachineClock, TheFramePeriodIsOneFrameInWholeNanoseconds) {
    EXPECT_EQ(kGameBoy.framePeriod(), 16'742'706ns);
    EXPECT_EQ(kNtsc.framePeriod(), 16'639'263ns);
    EXPECT_EQ(kPal.framePeriod(), 19'997'208ns);
    constexpr vm::MachineClock noClock{.hertzNumerator = 0, .hertzDivisor = 1,
                                       .cyclesPerFrame = 70'224};
    EXPECT_EQ(noClock.framePeriod(), 0ns);
}

TEST(MachineClock, OneOwnFrameIsWorthExactlyTheFrameCount) {
    const CycleDraw gb = kGameBoy.cyclesFor(16'742'706ns, 4'321);
    EXPECT_EQ(gb.cycles, 70'224u);
    EXPECT_EQ(gb.carryNs, 4'321u);
    const CycleDraw ntsc = kNtsc.cyclesFor(16'639'263ns, 4'321);
    EXPECT_EQ(ntsc.cycles, 357'366u);
    EXPECT_EQ(ntsc.carryNs, 4'321u);
    const CycleDraw pal = kPal.cyclesFor(19'997'208ns, 4'321);
    EXPECT_EQ(pal.cycles, 425'568u);
    EXPECT_EQ(pal.carryNs, 4'321u);
}

TEST(MachineClock, AnyOtherSpanIsTheRateWithTheFractionCarried) {
    const CycleDraw gb = kGameBoy.cyclesFor(16'666'667ns, 0);
    EXPECT_EQ(gb.cycles, 69'905u);
    EXPECT_EQ(gb.carryNs, 68'064'768u);
    const CycleDraw gbCarry = kGameBoy.cyclesFor(16'666'667ns, 999'999'999);
    EXPECT_EQ(gbCarry.cycles, 69'906u);
    EXPECT_EQ(gbCarry.carryNs, 68'064'767u);
    const CycleDraw ntsc = kNtsc.cyclesFor(16'666'667ns, 0);
    EXPECT_EQ(ntsc.cycles, 357'954u);
    EXPECT_EQ(ntsc.carryNs, 6'078'750'000u);
    const CycleDraw ntscForeign = kNtsc.cyclesFor(16'742'706ns, 0);
    EXPECT_EQ(ntscForeign.cycles, 359'587u);
    EXPECT_EQ(ntscForeign.carryNs, 7'292'500'000u);
}

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

TEST(MachineClock, AMillionForeignTicksNeverLoseACycle) {
    std::uint64_t total = 0;
    CycleDraw     draw{};
    for (std::uint64_t t = 1; t <= 1'000'000u; ++t) {
        draw = kNtsc.cyclesFor(std::chrono::nanoseconds{kHz60Ns}, draw.carryNs);
        total += draw.cycles;
        ASSERT_EQ(total, exactAfter(t)) << "tick " << t;
    }
    EXPECT_EQ(total, 357'954'552'613u);
    EXPECT_EQ(draw.carryNs, 7'000'000'000u);
}

TEST(MachineClock, NoSpanAndNoClockDrawNothing) {
    EXPECT_EQ(kGameBoy.cyclesFor(0ns, 4'321).cycles, 0u);
    EXPECT_EQ(kGameBoy.cyclesFor(0ns, 4'321).carryNs, 4'321u);
    EXPECT_EQ(kGameBoy.cyclesFor(-5ns, 4'321).cycles, 0u);
    EXPECT_EQ(kGameBoy.cyclesFor(-5ns, 4'321).carryNs, 4'321u);
    constexpr vm::MachineClock zeroNum{.hertzNumerator = 0, .hertzDivisor = 11,
                                       .cyclesPerFrame = 357'366};
    constexpr vm::MachineClock zeroDiv{.hertzNumerator = 236'250'000, .hertzDivisor = 0,
                                       .cyclesPerFrame = 357'366};
    EXPECT_EQ(zeroNum.cyclesFor(16'666'667ns, 4'321).cycles, 0u);
    EXPECT_EQ(zeroNum.cyclesFor(16'666'667ns, 4'321).carryNs, 4'321u);
    EXPECT_EQ(zeroDiv.cyclesFor(16'666'667ns, 4'321).cycles, 0u);
    EXPECT_EQ(zeroDiv.cyclesFor(16'666'667ns, 4'321).carryNs, 4'321u);
}

TEST(MachineClock, TheGameBoyCoreAnswersItsOwnClock) {
    const std::optional<vm::MachineClock> gb =
        vm::VmCoreAccess::clockOf(&detail::gameBoyCore, VMPlatform::GameBoy);
    const std::optional<vm::MachineClock> gbc =
        vm::VmCoreAccess::clockOf(&detail::gameBoyCore, VMPlatform::GameBoyColor);
    ASSERT_TRUE(gb.has_value());
    ASSERT_TRUE(gbc.has_value());
    EXPECT_EQ(*gb, kGameBoy);
    EXPECT_EQ(*gbc, kGameBoy);
}

TEST(MachineClock, ACoreThatKeepsNoClockAnswersNothing) {
    EXPECT_FALSE(testing::MockVmBackend{}.clock().has_value());
}

// ── A clock that is a ratio, through the governor ─────────────────────────────────────────────────

TEST(RunGovernor, ARatioClockOwesExactlyRateTimesTime) {
    RunGovernor   gov{236'250'000, 11};
    gov.restart(at(0));
    std::uint64_t owed = gov.owedThrough(at(250'000'000));
    owed               = gov.owedThrough(at(500'000'000));
    owed               = gov.owedThrough(at(750'000'000));
    owed               = gov.owedThrough(at(1'000'000'000));
    EXPECT_EQ(owed, 21'477'272u);

    RunGovernor loop{236'250'000, 11};
    loop.restart(at(0));
    std::int64_t now = 0;
    for (int i = 0; i < 10'000; ++i) {
        now += 3'337 + (i % 7) * 1'013;
        owed = loop.owedThrough(at(now));
    }
    EXPECT_EQ(now, 63'753'922);
    EXPECT_EQ(owed, 1'369'260u);
}

TEST(RunGovernor, TheTopFactorOverAWholeFoldIsCarriedWide) {
    RunGovernor gov{236'250'000, 11};
    gov.setFactor(1024, 1);
    gov.restart(at(0));
    EXPECT_EQ(gov.owedThrough(at(250'000'000)), 5'498'181'818u);
}

TEST(RunGovernor, TheWaitForOneFrameIsThatFramesPeriodRoundedUp) {
    RunGovernor ntsc{236'250'000, 11};
    EXPECT_EQ(ntsc.timeUntilOwed(357'366), 16'639'264ns);
    RunGovernor pal{21'281'370, 1};
    EXPECT_EQ(pal.timeUntilOwed(425'568), 19'997'209ns);
    RunGovernor gb{4'194'304, 1};
    EXPECT_EQ(gb.timeUntilOwed(70'224), 16'742'707ns);
}

TEST(RunGovernor, ADivisorOfOneIsTheWholeHertzClock) {
    RunGovernor oneArg{4'194'304};
    RunGovernor twoArg{4'194'304, 1};
    oneArg.setFactor(3, 7);
    twoArg.setFactor(3, 7);
    oneArg.restart(at(0));
    twoArg.restart(at(0));
    std::int64_t now = 0;
    for (int i = 0; i < 10'000; ++i) {
        now += 3'337 + (i % 7) * 1'013;
        const std::uint64_t a = oneArg.owedThrough(at(now));
        const std::uint64_t b = twoArg.owedThrough(at(now));
        ASSERT_EQ(a, b) << "fold " << i;
        ASSERT_EQ(oneArg.timeUntilOwed(a + 1'000), twoArg.timeUntilOwed(b + 1'000)) << "fold " << i;
    }
}

TEST(RunGovernor, RefusesAClockItCannotHold) {
    EXPECT_THROW(RunGovernor(1, 0), std::invalid_argument);
    EXPECT_THROW(RunGovernor(0, 1), std::invalid_argument);
    EXPECT_THROW(RunGovernor(1, 18'014'399), std::invalid_argument);
    EXPECT_NO_THROW(RunGovernor(1, 18'014'398));
}

// ── The generic host, paced from the machine's clock ──────────────────────────────────────────────

TEST(MachineOwnedPacing, ATickIsWorthTheMachinesOwnCyclesUnderAnyProfile) {
    Vm gbc{VMPlatform::GameBoyColor, TimingProfile{.tickPeriodNs = TickPeriodNs::Hz60, .cpu = {}}};
    EXPECT_EQ(VmTestAccess::tickBudget(gbc, 16'742'706ns), 70'224u);
    EXPECT_EQ(VmTestAccess::tickBudget(gbc, 16'666'667ns), 69'905u);
}

}  // namespace
}  // namespace retropp
