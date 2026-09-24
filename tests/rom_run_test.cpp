// The hosted cartridge runs: Vm::run / speed / stop, the governor that paces the machine against
// the wall clock, the per-step publish the game observes it through, and the write channel that
// crosses to its thread.
//
// The cartridges here are authored by these tests, byte for byte (tests/authored_cartridge.h), and
// their code is assembled by the engine's own SM83 assembler — an authored image is the only one
// with ground truth. Every running-machine case steps the run deterministically through the inline
// seam (src/vm/vm_testing.h); the one threaded case asserts liveness, never a wall-clock ratio,
// because how far a thread gets in a span is the host scheduler's property, not the engine's.

#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/memory_region.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/vm/run_governor.h"
#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

using vm::RunGovernor;
using vm::VmTestAccess;
using Clock = std::chrono::steady_clock;

// ── The governor's arithmetic (device-free) ─────────────────────────────────────────────────────

constexpr std::uint32_t kGbClock = 4'194'304;

Clock::time_point at(std::int64_t ns) { return Clock::time_point{std::chrono::nanoseconds{ns}}; }

TEST(RunGovernor, OwedAtUnityIsExactOverTenThousandFolds) {
    RunGovernor gov(kGbClock);
    gov.restart(at(0));
    // Awkward, coprime-with-nothing spans, folded one at a time; the closed form owes
    // floor(clock × total / 1e9) — a running carry means the two never drift apart.
    std::int64_t now = 0;
    std::uint64_t owed = 0;
    for (int i = 0; i < 10'000; ++i) {
        now += 3'337 + (i % 7) * 1'013;  // ns per fold, deliberately unround
        owed = gov.owedThrough(at(now));
    }
    const std::uint64_t closed =
        static_cast<std::uint64_t>(kGbClock) * static_cast<std::uint64_t>(now) / 1'000'000'000u;
    EXPECT_EQ(owed, closed);
}

TEST(RunGovernor, DoubledFactorOwesDoubleExactly) {
    RunGovernor unity(kGbClock);
    RunGovernor doubled(kGbClock);
    doubled.setFactor(2, 1);
    unity.restart(at(0));
    doubled.restart(at(0));
    std::int64_t now = 0;
    std::uint64_t owedUnity = 0;
    std::uint64_t owedDoubled = 0;
    for (int i = 0; i < 1'000; ++i) {
        now += 16'742'706;
        owedUnity   = unity.owedThrough(at(now));
        owedDoubled = doubled.owedThrough(at(now));
    }
    const std::uint64_t closed =
        2u * static_cast<std::uint64_t>(kGbClock) * static_cast<std::uint64_t>(now) /
        1'000'000'000u;
    EXPECT_EQ(owedDoubled, closed);
    // And double means double of the same wall time, up to the one cycle the carry holds back.
    EXPECT_GE(owedDoubled, 2 * owedUnity);
    EXPECT_LE(owedDoubled, 2 * owedUnity + 1);
}

TEST(RunGovernor, PausedFactorOwesNothingAndDropsThePausedSpan) {
    RunGovernor gov(kGbClock);
    gov.restart(at(0));
    const std::uint64_t before = gov.owedThrough(at(1'000'000));  // 1 ms at unity
    gov.setFactor(0, 1);
    // A long pause accrues nothing…
    EXPECT_EQ(gov.owedThrough(at(5'000'000'000)), before);
    // …and the paused span never surfaces as debt after unpausing: only post-unpause time owes.
    gov.setFactor(1, 1);
    const std::uint64_t after = gov.owedThrough(at(5'001'000'000));
    const std::uint64_t oneMsMore =
        static_cast<std::uint64_t>(kGbClock) * 2'000'000u / 1'000'000'000u;
    EXPECT_LE(after, oneMsMore + 1);
}

TEST(RunGovernor, FactorChangeKeepsTheSubCycleCarry) {
    // 2 Hz: one cycle per half second, so every 250 ms fold is a clean half cycle. Three folds at
    // unity accrue 1.5 cycles — 1 owed, a half carried. The factor then halves; six more folds are
    // 1.5 s of wall, 1.5 cycles at half speed, and with the carried half the total is exactly 3.
    // A carry dropped at the change lands on 2 instead.
    constexpr std::uint32_t kClock = 2;
    RunGovernor gov(kClock);
    gov.restart(at(0));
    std::uint64_t owed = 0;
    for (std::int64_t t = 250'000'000; t <= 750'000'000; t += 250'000'000) {
        owed = gov.owedThrough(at(t));
    }
    EXPECT_EQ(owed, 1u);  // 1.5 cycles → 1 owed, 0.5 carried
    gov.setFactor(1, 2);
    for (std::int64_t t = 1'000'000'000; t <= 2'250'000'000; t += 250'000'000) {
        owed = gov.owedThrough(at(t));
    }
    EXPECT_EQ(owed, 3u);
}

TEST(RunGovernor, StarvationBeyondTheFoldCapIsDroppedNotOwed) {
    RunGovernor gov(kGbClock);
    gov.restart(at(0));
    // One fold after two full seconds of silence: only the cap's 250 ms is owed.
    const std::uint64_t owed = gov.owedThrough(at(2'000'000'000));
    EXPECT_EQ(owed, static_cast<std::uint64_t>(kGbClock) / 4u);
}

TEST(RunGovernor, RefusesWhatItCannotMean) {
    EXPECT_THROW(RunGovernor(0), std::invalid_argument);          // no clock, no owing
    RunGovernor gov(kGbClock);
    EXPECT_THROW(gov.setFactor(1, 0), std::invalid_argument);     // a fraction of nothing
    EXPECT_THROW(gov.setFactor(2000, 1), std::invalid_argument);  // past the bound
    EXPECT_THROW(gov.setFactor(1, 2000), std::invalid_argument);
    gov.setFactor(0, 1);                                          // pause is a factor, not an error
    EXPECT_EQ(gov.factor(), (std::pair<std::uint32_t, std::uint32_t>{0u, 1u}));
}

// ── Authored, runnable cartridges ───────────────────────────────────────────────────────────────

// A cartridge whose entry jumps to `mainSource` assembled at $0150. The VBlank vector holds reti;
// a lone ret sits at $0900 so code that runs off into a still-mapped CGB overlay returns instead
// of running away.
std::vector<std::uint8_t> runnableCartridge(Vm& assembler, std::string_view mainSource,
                                            std::uint8_t cgbFlag) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(testing::kSmallestCartridge);
    rom[0x143] = cgbFlag;
    rom[0x040] = 0xD9;                                      // VBlank vector: reti
    rom[0x100] = 0x00;                                      // entry: nop
    rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;  // jp $0150
    rom[0x900] = 0xC9;                                      // ret past the CGB overlay window
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(mainSource));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

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

// Pins the boot mode from inside: the entry A and B into HRAM, then the WRAM-banking probe — two
// different banks written through SVBK and one read back. Banking works only in CGB mode, so the
// read answers $AA there and $BB anywhere the switch is inert.
constexpr std::string_view kModeProbeSource = R"(
    ldh [$FF80], a      ; the A the boot handed over
    ld a, b
    ldh [$FF81], a      ; the B the boot handed over
    ld a, $02
    ldh [$FF70], a      ; SVBK: bank 2
    ld a, $AA
    ld [$D000], a
    ld a, $03
    ldh [$FF70], a      ; SVBK: bank 3
    ld a, $BB
    ld [$D000], a
    ld a, $02
    ldh [$FF70], a      ; back to bank 2
    ld a, [$D000]
    ldh [$FF84], a      ; CGB mode: $AA — the banks are distinct; inert switch: $BB
done:
    jr done
)";

// Calls into the CGB boot overlay's window ($0200-$08FF): the sentinel routine lives at $0250 in
// the cartridge, so it only runs — and only sets its mark — when the overlay is unmapped.
constexpr std::string_view kOverlayProbeSource = R"(
    xor a
    ldh [$FF85], a
    call $0250
done:
    jr done
)";

// What the tests observe, declared once per machine before it runs.
struct Places {
    MemoryRegion hram;
    MemoryRegion wram;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(regions(
        region(&Places::hram, MemoryRegion{.at = 0xFF80, .size = 1, .count = 8}, "hram-observations"),
        region(&Places::wram, MemoryRegion{.at = 0xC000, .size = 1, .count = 4}, "wram-cells")));
}

std::uint8_t hram(Vm& machine, const RegionMapId<Places>& places, std::uint32_t index) {
    return machine.read(places, &Places::hram, index).at(0);
}

// ── Boot and the running loop (deterministic, through the inline seam) ──────────────────────────

TEST(RomRun, RunRefusesWhatCannotRun) {
    Vm::GBC bare;
    EXPECT_THROW(bare.run(), std::logic_error);  // nothing hosted

    Vm::GBC hosted;
    hosted.hostRom(runnableCartridge(hosted, kFrameCounterSource, 0x80));
    VmTestAccess::runInline(hosted);
    EXPECT_THROW(hosted.run(), std::logic_error);  // already running
    hosted.stop();
}

// A machine's speed is its core's own: a Game Boy handed a profile with no CPU block takes a speed
// factor and runs, and one step is one Game Boy frame.
TEST(RomRun, AMachineRunsAtItsOwnSpeedUnderAProfileWithNoCpuBlock) {
    Vm machine{VMPlatform::GameBoyColor, TimingProfile{TickPeriodNs::Hz60, std::nullopt}};
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    EXPECT_NO_THROW(machine.speed(1, 1));
    VmTestAccess::runInline(machine);
    for (int i = 0; i < 10; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    const std::uint8_t frames = hram(machine, places, 0);
    EXPECT_GE(frames, 8u);  // the bounds TheBootedImageRunsItsOwnMainLoop holds a Vm::GBC to
    EXPECT_LE(frames, 11u);
    machine.stop();
}

TEST(RomRun, TheBootedImageRunsItsOwnMainLoop) {
    // The headline: a commercial-shaped image — interrupt vector, halt loop, its own counter —
    // boots and LIVES. One step is one frame's cycles, so ten steps see about ten VBlanks; that
    // the counter moves at all is the PPU keeping frame time headless, and that it lands near ten
    // is the budget arithmetic holding.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    for (int i = 0; i < 10; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    const std::uint8_t frames = hram(machine, places, 0);
    EXPECT_GE(frames, 8u);
    EXPECT_LE(frames, 11u);
    machine.stop();
}

TEST(RomRun, ACgbFlaggedImageBootsInCgbMode) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x11);  // BOOTUP_A_CGB
    EXPECT_EQ(hram(machine, places, 1), 0x00);  // B: no DMG-compat checksum in CGB mode
    EXPECT_EQ(hram(machine, places, 4), 0xAA);  // WRAM banking works: CGB mode
    machine.stop();
}

TEST(RomRun, ADmgFlaggedImageBootsDmgCompatOnACgbMachine) {
    Vm::GBC machine;
    std::vector<std::uint8_t> rom = runnableCartridge(machine, kModeProbeSource, 0x00);
    rom[0x14B] = 0x01;  // old licensee: Nintendo — the firmware computes the title checksum
    for (std::size_t i = 0; i < 5; ++i) {
        rom[0x134 + i] = static_cast<std::uint8_t>("RETRO"[i]);
    }
    std::uint8_t checksum = 0;
    for (std::size_t i = 0x134; i <= 0x143; ++i) {
        checksum = static_cast<std::uint8_t>(checksum + rom[i]);
    }
    machine.hostRom(rom);
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x11);      // still the CGB machine's A
    EXPECT_EQ(hram(machine, places, 1), checksum);  // B carries the firmware's title checksum
    EXPECT_EQ(hram(machine, places, 4), 0xBB);      // WRAM banking inert: DMG compatibility
    machine.stop();
}

TEST(RomRun, ADmgMachineBootsDmgState) {
    Vm::GB machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x00));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x01);  // BOOTUP_A_DMG
    EXPECT_EQ(hram(machine, places, 1), 0x00);  // B: $0013's high byte
    EXPECT_EQ(hram(machine, places, 4), 0xBB);  // no WRAM banking on a DMG
    machine.stop();
}

TEST(RomRun, TheBootUnmapsTheOverlay) {
    // The sentinel routine lives at $0250 — inside the window the CGB boot overlay covers. Only an
    // unmapped overlay lets the cartridge's own bytes run there.
    Vm::GBC machine;
    std::vector<std::uint8_t> rom = runnableCartridge(machine, kOverlayProbeSource, 0x80);
    rom[0x250] = 0x3E; rom[0x251] = 0x5A;                   // ld a, $5A
    rom[0x252] = 0xE0; rom[0x253] = 0x85;                   // ldh [$FF85], a
    rom[0x254] = 0xC9;                                      // ret
    machine.hostRom(rom);
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 5), 0x5A);
    machine.stop();
}

TEST(RomRun, StopParksAndRunResumesWhereItParked) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    for (int i = 0; i < 5; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    const std::uint8_t parked = hram(machine, places, 0);
    machine.stop();
    // Running again is a resume: the counter grows from where it parked, so the image was neither
    // re-booted nor re-zeroed.
    VmTestAccess::runInline(machine);
    for (int i = 0; i < 5; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    EXPECT_GT(hram(machine, places, 0), parked);
    machine.stop();
    // reset() asks for a fresh boot: the counter starts over.
    machine.reset();
    VmTestAccess::runInline(machine);
    for (int i = 0; i < 3; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    EXPECT_LT(hram(machine, places, 0), parked);
    machine.stop();
}

// ── The publish and the write channel ───────────────────────────────────────────────────────────

TEST(RomRun, AWriteLandsAtTheStepBoundaryAndShowsInThatStepsPublish) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);  // the probe settles into its done loop
    const std::uint8_t before = machine.read(places, &Places::wram, 2).at(0);
    const std::uint8_t sentinel = 0x42;
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{sentinel}, 2);
    // Queued, not landed: the publish still answers the last step's capture.
    EXPECT_EQ(machine.read(places, &Places::wram, 2).at(0), before);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(machine.read(places, &Places::wram, 2).at(0), sentinel);
    machine.stop();
}

TEST(RomRun, WritesLandInIssueOrder) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x01}, 3);
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x02}, 3);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(machine.read(places, &Places::wram, 3).at(0), 0x02);  // the later write won
    machine.stop();
}

TEST(RomRun, AQueuedWriteStillLandsWhenTheMachineStops) {
    // Parking is a step boundary like any other: a write the game issued against a machine that will
    // not step again still lands, and is there to read back parked.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);  // the probe settles into its done loop
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x5A}, 1);
    machine.stop();
    EXPECT_EQ(machine.read(places, &Places::wram, 1).at(0), 0x5A);
}

TEST(RomRun, WritesQueuedBeforeAStopLandInIssueOrder) {
    // Two writes to one place and nothing left to step: the machine ends on the LAST one, which is
    // only true of a queue that keeps its order at the stop boundary. The pair reads differently the
    // other way round, so it tells the two apart.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x01}, 3);
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x02}, 3);
    machine.stop();
    EXPECT_EQ(machine.read(places, &Places::wram, 3).at(0), 0x02);
}

TEST(RomRun, AStoppedWriteDoesNotOutliveTheRunItWasIssuedIn) {
    // A write belongs to the run it was issued in. One issued while running, then a stop, then one
    // issued parked to the same place: the parked write is the later verb and stays the later verb,
    // including across a resume — the first has no run left to land in.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0xA1}, 2);
    machine.stop();
    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0xB2}, 2);
    EXPECT_EQ(machine.read(places, &Places::wram, 2).at(0), 0xB2);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(machine.read(places, &Places::wram, 2).at(0), 0xB2);
    machine.stop();
}

TEST(RomRun, AQueuedWriteMeetsItsWatchWhenTheMachineStops) {
    // The write that lands at the stop is the game's own verb, so a watch that asked for those is
    // offered it and its verdict is honoured: a veto keeps the byte the cell already holds.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    int                       fired  = 0;
    machine.registerWatches(watches(
        GuestWatch{.key     = "held cell",
                   .at      = MemoryRegion{.at = 0xC003, .size = 1},
                   .from    = AccessSource::GuestAndGame,
                   .onWrite = [&fired](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::veto();
                   }}));
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    const std::uint8_t held   = machine.read(places, &Places::wram, 3).at(0);
    const int          before = fired;

    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x77}, 3);
    machine.stop();

    EXPECT_EQ(fired, before + 1);
    EXPECT_EQ(machine.read(places, &Places::wram, 3).at(0), held);
}

TEST(RomRun, EveryStepPublishesExactlyOnce) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    declarePlaces(machine);
    VmTestAccess::runInline(machine);
    const std::uint32_t seq = VmTestAccess::publishSeq(machine);
    EXPECT_EQ(seq % 2u, 0u);  // stable between steps
    for (int i = 0; i < 3; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    EXPECT_EQ(VmTestAccess::publishSeq(machine), seq + 6u);  // two edges per publish
    machine.stop();
}

TEST(RomRun, AMidFlightPublishHoldsReadsOff) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    EXPECT_TRUE(VmTestAccess::readIsStable(machine));
    VmTestAccess::tornPublishBegin(machine);
    // Officially unstable: this is the state read() refuses to return from.
    EXPECT_FALSE(VmTestAccess::readIsStable(machine));
    VmTestAccess::tornPublishEnd(machine);
    EXPECT_TRUE(VmTestAccess::readIsStable(machine));
    // And a read now answers the completed capture, coherently.
    EXPECT_EQ(hram(machine, places, 0), 0x11);
    machine.stop();
}

TEST(RomRun, ARunningMachineRefusesWhatBelongsToItsThread) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kModeProbeSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    const MemoryRegion undeclared{.at = 0xC100, .size = 1};
    EXPECT_THROW((void)machine.read(undeclared), std::logic_error);
    EXPECT_THROW(machine.write(undeclared, std::vector<std::uint8_t>{0x00}), std::logic_error);
    EXPECT_THROW((void)declarePlaces(machine), std::logic_error);  // the set is fixed while running
    EXPECT_THROW(machine.reset(), std::logic_error);
    // And the declared verbs still validate their shapes.
    EXPECT_THROW(machine.write(places, &Places::wram, std::vector<std::uint8_t>{0, 0}, 0),
                 std::invalid_argument);                            // two bytes is not one entry
    EXPECT_THROW((void)machine.read(places, &Places::wram, 9), std::out_of_range);
    machine.stop();
    EXPECT_NO_THROW((void)machine.read(undeclared));  // stopped: places built on the spot are back
}

// ── The thread itself ───────────────────────────────────────────────────────────────────────────

TEST(RomRun, AThreadedRunLivesAndStops) {
    // Liveness, not cadence: the machine's own loop advances on its own thread, reads are answered
    // while it runs, and stop() has joined by return. How many frames a span yields belongs to the
    // host's scheduler and is asserted nowhere.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run();
    std::uint8_t frames = 0;
    for (int waited = 0; waited < 400 && frames == 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frames = hram(machine, places, 0);
    }
    machine.stop();
    EXPECT_GT(frames, 0u);
    EXPECT_EQ(machine.speed(), (std::pair<std::uint32_t, std::uint32_t>{1u, 1u}));
}

TEST(RomRun, AWriteQueuedOnAThreadedRunLandsWhenItStops) {
    // The stop boundary on the machine's own thread. Paced to a standstill there is no step boundary
    // left for the write to land at, so parking is the only one it can arrive at — and it does.
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kFrameCounterSource, 0x80));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run();

    std::uint8_t frames = 0;
    for (int waited = 0; waited < 400 && frames == 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frames = hram(machine, places, 0);
    }
    ASSERT_GT(frames, 0u);

    machine.speed(0, 1);
    bool settled = false;
    for (int waited = 0; waited < 400 && !settled; ++waited) {
        const std::uint8_t was = hram(machine, places, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        settled = hram(machine, places, 0) == was;
    }
    ASSERT_TRUE(settled);

    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x6C}, 0);
    machine.stop();

    EXPECT_EQ(machine.read(places, &Places::wram, 0).at(0), 0x6C);
}

}  // namespace
}  // namespace retropp
