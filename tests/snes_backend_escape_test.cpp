// Escapes on the SNES core: what an unwatched machine carries, that an armed address reports while the
// CPU runs, that the instruction there still executes exactly once, and what a replaced routine is on
// this console — a return standing in for the fetch that begins it, the image never written, an RTL for
// a routine a JSL enters, and one byte whichever address reaches it.
//
// The device-free suite (guest_escape_test.cpp) pins the surface on a machine with no CPU. These are the
// claims only the 65816 can answer. Every cartridge here is authored by these tests as 65816 source.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/snes.h"
#include "retropp/vm.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"
#include "src/vm/vm_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;
using vm::VmTestAccess;

// A one-bank LoROM cartridge whose reset code is `source`.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    for (const snaggletooth::assembler::Diagnostic& d : program.errors) {
        ADD_FAILURE() << "line " << d.line << ": " << d.message;
    }
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(range.start - 0x8000u));
    }
    return rom;
}

// Native mode, 8-bit registers, A cleared, then INC A and a branch back, forever. The escape at $00:8100
// reports exactly as often as INC A runs, so the two counts prove each other.
constexpr std::uint32_t kLoop = 0x008100;
constexpr std::string_view kLoopSource =
    "        ORG $00:8000\n"
    "        EMULATION\n"
    "        CLC\n"
    "        XCE\n"
    "        SEP #$30\n"
    "        A8\n"
    "        X8\n"
    "        LDA #$00\n"
    "        JMP !loop\n"
    "        ORG $00:8100\n"
    "loop:   INC A\n"
    "        BRA loop\n";

SnesBackend& booted(SnesBackend& backend, std::string_view source) {
    backend.loadRom(cartridgeFrom(source));
    backend.bootHostedRom();
    return backend;
}

// ── What an unwatched machine carries ───────────────────────────────────────────────────────────

TEST(SnesEscapes, AMachineWatchingNothingCarriesNoWatcher) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    backend.setEscapeSink([](std::uint32_t) {});

    EXPECT_FALSE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
    backend.runForCycles(4'000);
    EXPECT_FALSE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
}

TEST(SnesEscapes, ArmingAnAddressInstallsTheWatcherAndDisarmingItRemovesIt) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    backend.setEscapeSink([](std::uint32_t) {});

    backend.armEscape(kLoop, false);
    EXPECT_TRUE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
    backend.disarmEscape(kLoop);
    EXPECT_FALSE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
}

TEST(SnesEscapes, AnArmedAddressWithNowhereToReportInstallsNothing) {
    SnesBackend backend;
    booted(backend, kLoopSource);

    backend.armEscape(kLoop, false);  // armed, but no sink: nothing could be told about it
    EXPECT_FALSE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
}

// The watcher and the armed set are the host's, not the machine's: a boot builds a fresh machine, and the
// escape armed before it is still armed after.
TEST(SnesEscapes, ABootKeepsWhatWasArmed) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kLoopSource));
    int fired = 0;
    backend.setEscapeSink([&](std::uint32_t) { ++fired; });
    backend.armEscape(kLoop, false);

    backend.bootHostedRom();
    EXPECT_TRUE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
    backend.runForCycles(4'000);
    EXPECT_GT(fired, 0);
}

// ── Firing, and what happens to the instruction that was reached ────────────────────────────────

TEST(SnesEscapes, AnArmedAddressReportsItselfWhileTheCpuRuns) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    int           fired = 0;
    std::uint32_t seen  = 0;
    backend.setEscapeSink([&](std::uint32_t address) {
        ++fired;
        seen = address;
    });
    backend.armEscape(kLoop, false);

    backend.runForCycles(4'000);
    EXPECT_GT(fired, 0);
    EXPECT_EQ(seen, kLoop);
}

// INC A runs exactly as many times as the address reported: observed, never replaced.
TEST(SnesEscapes, TheInstructionAtAnArmedAddressStillExecutesExactlyOncePerReport) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    int fired = 0;
    backend.setEscapeSink([&](std::uint32_t) { ++fired; });
    backend.armEscape(kLoop, false);

    backend.runForCycles(4'000);
    ASSERT_GT(fired, 0);
    ASSERT_LT(fired, 256);  // A is eight bits; the budget stays inside one wrap
    EXPECT_EQ(SnesBackendTestAccess::state(backend).cpu.a & 0xFF, fired);
}

// The same loop with nothing armed advances A identically: the host runs on its own time.
TEST(SnesEscapes, ArmingAnAddressCostsTheGuestNoneOfItsOwnTime) {
    const auto count = [](bool armed) {
        SnesBackend backend;
        booted(backend, kLoopSource);
        if (armed) {
            backend.setEscapeSink([](std::uint32_t) {});
            backend.armEscape(kLoop, false);
        }
        backend.runForCycles(4'000);
        return SnesBackendTestAccess::state(backend).cpu.a & 0xFF;
    };
    EXPECT_EQ(count(true), count(false));
}

// Two addresses that reach one byte are one place: $80:8100 is the cartridge's $00:8100 through the
// banks the map repeats it in, and an escape armed there hears the loop the program runs in bank $00.
TEST(SnesEscapes, AnEscapeAtOneAliasFiresForTheCodeRunningThroughAnother) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    int           fired = 0;
    std::uint32_t seen  = 0;
    backend.setEscapeSink([&](std::uint32_t address) {
        ++fired;
        seen = address;
    });
    backend.armEscape(0x808100, false);

    backend.runForCycles(4'000);
    EXPECT_GT(fired, 0);
    EXPECT_EQ(seen, 0x808100u);  // reported as it was armed
}

TEST(SnesEscapes, AnEscapeNamesCodeAndOneByteStandsOneAnswer) {
    SnesBackend backend;
    booted(backend, kLoopSource);
    EXPECT_THROW(backend.armEscape(0x004210, false), std::invalid_argument);            // a register
    EXPECT_THROW(backend.armEscape(0x006000, false), std::invalid_argument);            // open bus
    EXPECT_THROW(backend.armEscape(snes::videoRam(0), false), std::invalid_argument);   // not on the bus

    backend.armEscape(kLoop, true);
    EXPECT_THROW(backend.armEscape(0x808100, false), std::invalid_argument);  // the same byte, answered otherwise
    backend.armEscape(0x808100, true);                                        // the same answer is the same place
}

// ── Through the public surface, on a cartridge that is running ──────────────────────────────────

TEST(SnesEscapes, AnEscapeFiresOnTheRunningCartridge) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kLoopSource));
    int fired = 0;
    machine.registerEscapes(escapes(GuestEscape{
        .key = "loop", .at = kLoop, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_GT(fired, 0);
}

TEST(SnesEscapes, AnEscapeSwitchedOffDoesNotFireOnTheRunningCartridge) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kLoopSource));
    int fired = 0;
    machine.registerEscapes(escapes(GuestEscape{
        .key = "loop", .at = kLoop, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));
    machine.escapes()["loop"].armed(false);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(fired, 0);
}

// ── Replacing a routine: `.replaces` answers in the routine's own calling convention ────────────

// A loop that carries a seed to its own routine in X and stores what comes back in A; the routine's own
// rule doubles it. Each time round it also reads the routine's first byte as data.
constexpr std::uint32_t kRule   = 0x008180;
constexpr std::uint32_t kSeed   = 0x7E0030;
constexpr std::uint32_t kResult = 0x7E0031;
constexpr std::uint32_t kPeeked = 0x7E0032;
constexpr std::string_view kCallingSource =
    "        ORG $00:8000\n"
    "        EMULATION\n"
    "        CLC\n"
    "        XCE\n"
    "        SEP #$30\n"
    "        A8\n"
    "        X8\n"
    "        JMP !loop\n"
    "        ORG $00:8100\n"
    "loop:   LDX $30\n"
    "        JSR !rule\n"
    "        STA $31\n"
    "        LDA !rule\n"
    "        STA $32\n"
    "        BRA loop\n"
    "        ORG $00:8180\n"
    "rule:   TXA\n"
    "        ASL A\n"
    "        RTS\n";

struct Places {
    MemoryRegion seed;
    MemoryRegion result;
    MemoryRegion peeked;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(
        regions(region(&Places::seed, MemoryRegion{.at = kSeed, .size = 1}, "seed"),
                region(&Places::result, MemoryRegion{.at = kResult, .size = 1}, "result"),
                region(&Places::peeked, MemoryRegion{.at = kPeeked, .size = 1}, "peeked")));
}

GuestEscape answerTheRule(std::uint32_t at) {
    return GuestEscape{
        .key      = "answer the rule",
        .at       = at,
        .replaces = routine(RoutineBinding{.inputs = {snes::X}, .output = snes::A},
                            [](std::uint16_t seed) -> std::uint8_t {
                                return static_cast<std::uint8_t>(seed + 100);
                            })};
}

TEST(SnesEscapes, UnreplacedTheCartridgeAnswersWithItsOwnRule) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCallingSource));
    const auto places = declarePlaces(machine);

    VmTestAccess::runInline(machine);
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(machine.read(places, &Places::result).at(0), 42);
}

// The engine reads X out of the machine the guest's caller loaded, calls the native function, and writes
// its answer into A — where the caller was always going to look — in the same step.
TEST(SnesEscapes, AReplacementAnswersInTheRoutinesOwnRegisters) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCallingSource));
    const auto places = declarePlaces(machine);
    machine.registerEscapes(escapes(answerTheRule(kRule)));

    VmTestAccess::runInline(machine);
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(machine.read(places, &Places::result).at(0), 121);  // the native rule, not 42
}

// The return stands in for the one fetch that begins the routine; the image is never written. The guest's
// own data read of that byte answers the cartridge's TXA ($8A), and so does the machine's copy of it.
TEST(SnesEscapes, AReplacedRoutinesByteIsNeverWritten) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCallingSource));
    const auto places = declarePlaces(machine);
    machine.registerEscapes(escapes(answerTheRule(kRule)));

    VmTestAccess::runInline(machine);
    machine.write(places, &Places::seed, std::vector<std::uint8_t>{21});
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(machine.read(places, &Places::result).at(0), 121);
    EXPECT_EQ(machine.read(places, &Places::peeked).at(0), 0x8A);
    EXPECT_EQ(machine.read(MemoryRegion{.at = kRule, .size = 1}).at(0), 0x8A);

    SnesBackend backend;
    booted(backend, kCallingSource);
    backend.armEscape(kRule, true);
    EXPECT_EQ(SnesBackendTestAccess::peek(backend, kRule), 0x8A);
}

// A routine a JSL enters leaves with an RTL, three bytes off the stack. Replaced through snes::rtl, the
// stand-in is an RTL, so the caller resumes after its JSL with the stack where it was; the loop counts on.
constexpr std::string_view kFarCallSource =
    "        ORG $00:8000\n"
    "        EMULATION\n"
    "        CLC\n"
    "        XCE\n"
    "        SEP #$30\n"
    "        A8\n"
    "        X8\n"
    "        STZ $40\n"
    "        JMP !loop\n"
    "        ORG $00:8100\n"
    "loop:   JSL $00:8180\n"
    "        INC $40\n"
    "        BRA loop\n"
    "        ORG $00:8180\n"
    "        A8\n"
    "far:    LDA #$FF\n"
    "        STA $41\n"
    "        RTL\n";

TEST(SnesEscapes, AnRtlEntryStandsAnRtlIn) {
    SnesBackend backend;
    booted(backend, kFarCallSource);

    // The stack pointer each time the routine is entered: a stand-in that took the wrong number of bytes
    // back off would move it every time round.
    std::vector<std::uint16_t> entered;
    backend.setEscapeSink([&](std::uint32_t) { entered.push_back(SnesBackendTestAccess::state(backend).cpu.s); });
    backend.armEscape(snes::rtl(0x008180), true);
    backend.runForCycles(4'000);

    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    ASSERT_GT(entered.size(), 5u);
    EXPECT_TRUE(std::all_of(entered.begin(), entered.end(), [&](std::uint16_t s) { return s == entered[0]; }));
    EXPECT_EQ(state.wram[0x41], 0x00);  // the body never ran
    EXPECT_GE(state.wram[0x40] + 1u, entered.size());  // and the caller carried on after each JSL
}

TEST(SnesEscapes, DeclaringEscapesOnARunningMachineIsRefused) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kLoopSource));
    VmTestAccess::runInline(machine);

    EXPECT_THROW(machine.registerEscapes(escapes(GuestEscape{
                     .key = "loop", .at = kLoop, .handler = [](Vm&, std::uint32_t) {}})),
                 std::logic_error);
    machine.stop();
}

}  // namespace
}  // namespace retropp
