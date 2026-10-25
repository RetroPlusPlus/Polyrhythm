// A hosted cartridge advanced by the engine's tick: Vm::run(Advance::OnTick), the budget one tick is
// worth at the current speed factor, and what the machine does between ticks.
//
// The arithmetic cases walk the budget one tick at a time through the internal seam
// (src/vm/vm_testing.h), because the exactness claim is about the cycle count itself and no guest
// observable is that fine. The behavioral cases use cartridges these tests author byte for byte
// (tests/authored_cartridge.h), assembled by the engine's own SM83 assembler — an authored image is
// the only one with ground truth.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/memory_region.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

using vm::VmTestAccess;

// The machine every case here runs, read from the profile rather than restated as literals.
const TimingProfile        kProfile = TimingProfile::GameBoyColor;
const std::chrono::nanoseconds kTick = kProfile.tickPeriod();
const std::uint64_t        kFrame   = kProfile.cpu->cyclesPerFrame;
const std::uint64_t        kClockHz = kProfile.cpu->cpuClockHz;

// ── What a tick is worth (device-free) ──────────────────────────────────────────────────────────

TEST(TickAdvance, UnityDrawsTheStoredFrameCountEveryTick) {
    // At the machine's own cadence the draw is the stored hardware fact, and the factor at unity is
    // the identity — so every tick is worth exactly one frame, with nothing accumulating either way.
    Vm::GBC machine;
    for (int i = 0; i < 10'000; ++i) {
        EXPECT_EQ(VmTestAccess::tickBudget(machine, kTick), kFrame);
    }
}

TEST(TickAdvance, TheFactorScalesWhatATickIsWorth) {
    Vm::GBC doubled;
    doubled.speed(2, 1);
    EXPECT_EQ(VmTestAccess::tickBudget(doubled, kTick), 2 * kFrame);

    Vm::GBC halved;
    halved.speed(1, 2);
    EXPECT_EQ(VmTestAccess::tickBudget(halved, kTick), kFrame / 2);  // 70'224 halves exactly

    Vm::GBC paused;
    paused.speed(0, 1);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(VmTestAccess::tickBudget(paused, kTick), 0u);
    }
    // Unpausing owes nothing for the paused span and resumes at full value.
    paused.speed(1, 1);
    EXPECT_EQ(VmTestAccess::tickBudget(paused, kTick), kFrame);
}

TEST(TickAdvance, AFactorThatDoesNotDivideCarriesItsRemainder) {
    // Five does not divide a frame, so each tick leaves a fraction behind. Kept, the five ticks sum
    // to exactly one frame; rounded away, each group is four cycles short.
    Vm::GBC machine;
    machine.speed(1, 5);
    std::uint64_t total = 0;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t budget = VmTestAccess::tickBudget(machine, kTick);
        EXPECT_LE(budget, kFrame / 5 + 1);  // never off its true value by more than a cycle
        EXPECT_GE(budget, kFrame / 5);
        total += budget;
    }
    EXPECT_EQ(total, kFrame * 10'000 / 5);
}

TEST(TickAdvance, AFactorChangeReDenominatesTheCarry) {
    // A thousand changes back and forth. A carry dropped at each change loses up to a cycle a time,
    // so the total falls a long way behind the exact rational sum; carried across, it stays within
    // the one cycle a running remainder can hold back.
    Vm::GBC machine;
    std::uint64_t total = 0;
    int           fifths = 0;
    int           thirds = 0;
    for (int i = 0; i < 1'000; ++i) {
        machine.speed(1, 5);
        total += VmTestAccess::tickBudget(machine, kTick);
        ++fifths;
        machine.speed(1, 3);
        total += VmTestAccess::tickBudget(machine, kTick);
        ++thirds;
    }
    const std::uint64_t exact = kFrame * fifths / 5 + kFrame * thirds / 3;
    EXPECT_LE(total, exact);
    EXPECT_GE(total + 1, exact);
}

TEST(TickAdvance, AForeignCadenceCarriesTheNanosecondRemainder) {
    // A machine ticked at a period that is not its own has no frame count to reach for, so the clock
    // rate answers and the sub-cycle remainder carries. Ten thousand milliseconds of ticks are worth
    // exactly ten seconds of this machine's cycles.
    Vm::GBC machine;
    constexpr std::chrono::nanoseconds kMillisecond{1'000'000};
    std::uint64_t total = 0;
    for (int i = 0; i < 10'000; ++i) {
        total += VmTestAccess::tickBudget(machine, kMillisecond);
    }
    EXPECT_EQ(total, kClockHz * 10'000ull * 1'000'000ull / 1'000'000'000ull);
}

// ── Authored, runnable cartridges ───────────────────────────────────────────────────────────────

// Counts frames: VBlank-only interrupts, halt in a loop, one HRAM increment per wake.
constexpr std::string_view kFrameCounterSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter
    ldh [$FF0F], a      ; start counting from the NEXT VBlank, not a pending one
    ei
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a
    jr loop
)";

std::vector<std::uint8_t> runnableCartridge(Vm& assembler, std::string_view mainSource) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(testing::kSmallestCartridge);
    rom[0x143] = 0x80;
    rom[0x040] = 0xD9;                                       // VBlank vector: reti
    rom[0x100] = 0x00;                                       // entry: nop
    rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;  // jp $0150
    rom[0x900] = 0xC9;                                       // ret past the CGB overlay window
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(mainSource));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

struct Places {
    MemoryRegion hram;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(regions(region(
        &Places::hram, MemoryRegion{.at = 0xFF80, .size = 1, .count = 8}, "hram-observations")));
}

std::uint8_t frames(Vm& machine, const RegionMapId<Places>& places) {
    return machine.read(places, &Places::hram, 0).at(0);
}

// ── The machine under the engine's tick ─────────────────────────────────────────────────────────

TEST(TickAdvance, EachTickAdvancesTheGuestByOneFrame) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    // Nothing is asserted before the first tick: power-on RAM is seeded from a PRNG, so the counter
    // holds whatever the machine came up with until the guest's own init has run.
    for (int i = 1; i <= 5; ++i) {
        machine.advanceTick();
        EXPECT_EQ(frames(machine, places), i);
    }
    machine.stop();
}

TEST(TickAdvance, TheFactorScalesWhatATickAdvances) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.speed(2, 1);
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 4);  // two ticks, two frames' worth each
    machine.stop();
}

TEST(TickAdvance, EachTickPublishesExactlyOnce) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    (void)declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    const std::uint32_t seq = VmTestAccess::publishSeq(machine);
    machine.advanceTick();
    EXPECT_EQ(VmTestAccess::publishSeq(machine), seq + 2);
    machine.advanceTick();
    EXPECT_EQ(VmTestAccess::publishSeq(machine), seq + 4);
    machine.stop();
}

TEST(TickAdvance, AMachineOnItsOwnClockRefusesTheTick) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    (void)declarePlaces(machine);
    machine.run();  // its own thread, its own clock
    EXPECT_THROW(machine.advanceTick(), std::logic_error);
    machine.stop();
    EXPECT_NO_THROW(machine.advanceTick());  // stopped: the clock verb is the caller's again
}

TEST(TickAdvance, AHostedMachineThatIsNotRunningAdvancesItsClockAsBefore) {
    // Ticking a cartridge that has not been run advances the machine's clock and nothing else — no
    // boot, no publish, no step.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    EXPECT_NO_THROW(machine.advanceTick());
}

TEST(TickAdvance, TheRunningRefusalsStandUnderTheTick) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    (void)declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    EXPECT_THROW(machine.reset(), std::logic_error);
    EXPECT_THROW((void)declarePlaces(machine), std::logic_error);
    EXPECT_THROW(machine.hostRom(std::vector<std::uint8_t>{0x00}), std::logic_error);
    machine.stop();
}

TEST(TickAdvance, StoppingParksTheMachineAndResetBootsItFresh) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 2);
    machine.stop();

    machine.run(Vm::Advance::OnTick);  // resumes where it parked
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 3);
    machine.stop();

    machine.reset();
    machine.run(Vm::Advance::OnTick);  // a fresh boot starts the count again
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 1);
    machine.stop();
}

TEST(TickAdvance, AWriteIsNotVisibleToTheReadThatFollowsIt) {
    // One thread does not buy read-your-writes. A read answers from the step's publish, so a write
    // issued between ticks is invisible until the next tick publishes — the same terms as the other
    // mode. (What this pins is the publish. Whether the write is queued to the boundary or applied
    // at once is not distinguishable from here, because the publish lags either way.)
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 1);
    machine.write(places, &Places::hram, std::vector<std::uint8_t>{0x40}, 0);
    EXPECT_EQ(frames(machine, places), 1);      // the read that follows the write does not see it
    machine.advanceTick();
    EXPECT_EQ(frames(machine, places), 0x41);   // landed at the boundary, then the guest counted on
    machine.stop();
}

TEST(TickAdvance, TwoMachinesGivenTheSameTicksHoldTheSameState) {
    // The property the mode exists for. Same image, same ticks, same factor — same bytes, with no
    // wall clock anywhere in the answer.
    Vm::GBC a;
    Vm::GBC b;
    a.hostRom(runnableCartridge(a, kFrameCounterSource));
    b.hostRom(runnableCartridge(b, kFrameCounterSource));
    const RegionMapId<Places> pa = declarePlaces(a);
    const RegionMapId<Places> pb = declarePlaces(b);
    a.speed(3, 2);
    b.speed(3, 2);
    a.run(Vm::Advance::OnTick);
    b.run(Vm::Advance::OnTick);
    for (int i = 0; i < 40; ++i) {
        a.advanceTick();
        b.advanceTick();
        ASSERT_EQ(a.read(pa, &Places::hram, 0), b.read(pb, &Places::hram, 0)) << "tick " << i;
    }
    EXPECT_NE(frames(a, pa), 0);  // and it actually ran
    a.stop();
    b.stop();
}

}  // namespace
}  // namespace retropp
