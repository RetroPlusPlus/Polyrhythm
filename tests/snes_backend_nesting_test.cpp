// Calling the SNES cartridge's own routines on the real core: what the interrupted instruction finds when
// it resumes, what the call spends of the guest's time, a routine that runs away, a routine that escapes
// into a handler that calls another, an interrupt arriving inside a routine, whose stack a call uses, a
// routine in work RAM, which thread may call, and a replacement answering with the cartridge's own code.
//
// The device-free suite (guest_nesting_test.cpp) pins the surface on a machine with no CPU. These are the
// claims only the 65816 can answer. Every cartridge here is authored by these tests as 65816 source, and
// running machines are stepped deterministically through the inline seam (src/vm/vm_testing.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
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
#include "src/vm/vm_backend.h"
#include "src/vm/vm_testing.h"

namespace retropp {
namespace {

using vm::CallStack;
using vm::SnesBackend;
using vm::SnesBackendTestAccess;
using vm::VmTestAccess;

constexpr auto id = [](snes::Reg r) { return static_cast<std::uint16_t>(r); };

// A one-bank LoROM cartridge: native mode and 8-bit registers, then `body` at $00:8100, with the native
// NMI vector at $00:8200.
std::vector<std::uint8_t> cartridge(std::string_view body) {
    const std::string source = "        ORG $00:8000\n"
                               "        EMULATION\n"
                               "        CLC\n"
                               "        XCE\n"
                               "        SEP #$30\n"
                               "        A8\n"
                               "        X8\n"
                               "        JMP !main\n"
                               "        ORG $00:8100\n"
                               "        A8\n"
                               "        X8\n"
                               "main:\n" +
                               std::string(body);
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
    rom[0x7FC0u + 0x2Au] = 0x00;  // native NMI vector -> $8200
    rom[0x7FC0u + 0x2Bu] = 0x82;
    return rom;
}

const RoutineBinding kPlain{.isa = Isa::Wdc65816};

std::uint8_t byteAt(Vm& machine, std::uint32_t at) {
    return machine.read(MemoryRegion{.at = at, .size = 1}).at(0);
}

void stepParked(Vm& machine, int steps) {
    VmTestAccess::runInline(machine);
    for (int i = 0; i < steps; ++i) {
        VmTestAccess::stepOnce(machine);
    }
    machine.stop();
}

// ── The interrupted instruction finds what it left ──────────────────────────────────────────────

// A hundred rounds, each adding three to a checksum the guest keeps in X across a call that is escaped —
// and, inside that escape, a routine of the guest's own that sets every register it can reach to $FF,
// the direct page included. The round counter is Y, so a file that did not come back would not even
// finish counting.
constexpr std::string_view kChecksum =
    "        LDY #$64\n"
    "        LDX #$00\n"
    "        STZ $30\n"
    "loop:   JSR !round\n"
    "        TXA\n"
    "        CLC\n"
    "        ADC #$03\n"
    "        TAX\n"
    "        DEY\n"
    "        BNE loop\n"
    "        STX $30\n"           // 300 & $FF = 44, and only if X survived every round
    "done:   BRA done\n"
    "        ORG $00:8180\n"
    "        A8\n"
    "round:  LDA #$2A\n"          // escaped here
    "        STA $32\n"
    "        RTS\n"
    "        ORG $00:81C0\n"
    "clobber: REP #$30\n"
    "        A16\n"
    "        X16\n"
    "        LDA #$FFFF\n"
    "        TAX\n"
    "        TAY\n"
    "        TCD\n"
    "        SEP #$30\n"
    "        A8\n"
    "        X8\n"
    "        RTS\n";

TEST(SnesNesting, TheInterruptedFrameComesBackExactlyAsItWas) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kChecksum));
    auto clobber = machine.bindRoutine<void()>(0x0081C0, kPlain);
    int  fired   = 0;
    machine.registerEscapes(escapes(GuestEscape{.key = "round", .at = 0x008180, .handler = [&](Vm&, std::uint32_t) {
                                                    ++fired;
                                                    clobber();
                                                }}));

    stepParked(machine, 2);
    EXPECT_EQ(fired, 100);                     // the escape fired every round
    EXPECT_EQ(byteAt(machine, 0x7E0030), 44);  // and X carried the checksum through all of them
    EXPECT_EQ(byteAt(machine, 0x7E0032), 0x2A);  // the escaped load kept its own operand
}

// ── What the call spends ────────────────────────────────────────────────────────────────────────

// Two machines on one cartridge, the same budget each; one takes a call between its two runs. The call's
// cycles come out of the budget the host runs next, so both clocks end together and the one that was
// called counted less of its own.
constexpr std::string_view kCounting =
    "        REP #$20\n"
    "        A16\n"
    "        STZ $30\n"
    "loop:   INC $30\n"
    "        BRA loop\n"
    "        ORG $00:8180\n"
    "        A8\n"
    "slow:   NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        NOP\n"
    "        RTS\n";

TEST(SnesNesting, ACallSpendsTheGuestsOwnTime) {
    const auto counted = [](bool called) {
        SnesBackend backend;
        backend.loadRom(cartridge(kCounting));
        backend.bootHostedRom();
        backend.runForCycles(2'000);
        if (called) {
            backend.callInContext(0x008180, {}, CallStack::Guest, 1'000, {});
        }
        backend.runForCycles(20'000);
        const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
        return std::pair{state.master, static_cast<unsigned>(state.wram[0x30] | state.wram[0x31] << 8)};
    };
    const auto [calledMaster, calledCount] = counted(true);
    const auto [plainMaster, plainCount]   = counted(false);
    EXPECT_LT(calledMaster > plainMaster ? calledMaster - plainMaster : plainMaster - calledMaster, 64u);
    EXPECT_LT(calledCount, plainCount);
}

// A routine that never returns is abandoned at its guard, and the frame still comes back.
constexpr std::string_view kRunaway =
    "        LDY #$04\n"
    "loop:   JSR !round\n"
    "        DEY\n"
    "        BNE loop\n"
    "done:   BRA done\n"
    "        ORG $00:8180\n"
    "round:  NOP\n"
    "        RTS\n"
    "        ORG $00:81A0\n"
    "        A8\n"
    "spin:   LDA #$FF\n"
    "        TAX\n"
    "        TAY\n"
    "self:   BRA self\n";

TEST(SnesNesting, ARoutineThatRunsAwayIsAbandonedAndTheFrameStillComesBack) {
    SnesBackend backend;
    backend.loadRom(cartridge(kRunaway));
    backend.bootHostedRom();

    int           fires   = 0;
    std::uint64_t beforeY = 0, afterY = 0, beforeS = 0, afterS = 0;
    backend.setEscapeSink([&](std::uint32_t) {
        const bool first = fires++ == 0;
        if (first) {
            beforeY = backend.readRegister(id(snes::Reg::Y));
            beforeS = backend.readRegister(id(snes::Reg::S));
        }
        backend.callInContext(0x0081A0, {}, CallStack::Guest, 50, {});
        if (first) {
            afterY = backend.readRegister(id(snes::Reg::Y));
            afterS = backend.readRegister(id(snes::Reg::S));
        }
    });
    backend.armEscape(0x008180, false);
    backend.runForCycles(20'000);

    EXPECT_EQ(fires, 4);         // the guest's own loop finished: every abandoned call still ended
    EXPECT_EQ(beforeY, 4u);      // its round counter, before the call
    EXPECT_EQ(afterY, beforeY);  // and back again, though the routine set it to $FF
    EXPECT_EQ(afterS, beforeS);
}

// ── Depth ───────────────────────────────────────────────────────────────────────────────────────

// A handler calls one of the guest's routines; an instruction inside THAT routine escapes, and its
// handler calls another. Each answer reaches its own caller, and the guest's own counting is untouched.
constexpr std::string_view kDepth =
    "        LDY #$08\n"
    "        STZ $30\n"
    "loop:   JSR !outer\n"
    "        INC $30\n"
    "        DEY\n"
    "        BNE loop\n"
    "done:   BRA done\n"
    "        ORG $00:8180\n"
    "outer:  NOP\n"               // escaped: the handler here calls the first routine
    "        RTS\n"
    "        ORG $00:81A0\n"
    "first:  NOP\n"
    "        NOP\n"               // escaped: the handler here calls the second routine
    "        LDA $33\n"
    "        ASL A\n"
    "        RTS\n"
    "        ORG $00:81C0\n"
    "        A8\n"
    "second: LDA #$07\n"
    "        STA $33\n"
    "        RTS\n";

TEST(SnesNesting, AHandlersRoutineMayEscapeIntoAHandlerThatCallsAnotherRoutine) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kDepth));
    auto first  = machine.bindRoutine<std::uint8_t()>(0x0081A0, RoutineBinding{.output = snes::A, .isa = Isa::Wdc65816});
    auto second = machine.bindRoutine<void()>(0x0081C0, kPlain);

    std::vector<std::uint8_t> answers;
    machine.registerEscapes(escapes(
        GuestEscape{.key = "outermost", .at = 0x008180, .handler = [&](Vm&, std::uint32_t) { answers.push_back(first()); }},
        GuestEscape{.key = "inside the first routine", .at = 0x0081A1, .handler = [&](Vm&, std::uint32_t) { second(); }}));

    stepParked(machine, 2);
    ASSERT_EQ(answers.size(), 8u);
    for (const std::uint8_t a : answers) {
        EXPECT_EQ(a, 14u);  // the second routine's 7, doubled by the first
    }
    EXPECT_EQ(byteAt(machine, 0x7E0030), 8);  // the guest counted its own rounds regardless
}

// An escape's handler is told between instructions, so a call from inside it is the same call: the
// routine runs, its answer is read before the file goes back, and the escaped instruction then runs once
// under the guest's own registers.
TEST(SnesNesting, ACallFromInsideAnEscapeIsTheSameCall) {
    SnesBackend backend;
    backend.loadRom(cartridge("        STZ $30\n"
                              "loop:   INC $30\n"
                              "        BRA loop\n"
                              "        ORG $00:8180\n"
                              "        A8\n"
                              "answer: LDA #$77\n"
                              "        RTS\n"));
    backend.bootHostedRom();

    std::vector<std::uint64_t> answers;
    backend.setEscapeSink([&](std::uint32_t) {
        backend.callInContext(0x008180, {}, CallStack::Guest, 100,
                              [&] { answers.push_back(backend.readRegister(id(snes::Reg::A))); });
    });
    backend.armEscape(0x008102, false);  // INC $30
    backend.runForCycles(4'000);

    ASSERT_GT(answers.size(), 0u);
    ASSERT_LT(answers.size(), 256u);
    EXPECT_TRUE(std::all_of(answers.begin(), answers.end(), [](std::uint64_t a) { return a == 0x77; }));
    // INC ran once per call — the last one too, unless the budget ended part-way through it.
    const std::size_t counted = SnesBackendTestAccess::state(backend).wram[0x30];
    EXPECT_TRUE(counted == answers.size() || counted + 1 == answers.size());
}

// ── An interrupt arriving inside a routine ──────────────────────────────────────────────────────

// The vertical-blank NMI counts in $33. A routine long enough to outlast a frame records the count on the
// way in and on the way out.
constexpr std::string_view kInterrupt =
    "        STZ $33\n"
    "        LDA #$80\n"
    "        STA !$4200\n"            // NMITIMEN: the vertical-blank NMI
    "loop:   JSR !tick\n"
    "        BRA loop\n"
    "        ORG $00:8180\n"
    "tick:   NOP\n"
    "        RTS\n"
    "        ORG $00:81A0\n"
    "slow:   LDA $33\n"
    "        STA $34\n"
    "        REP #$10\n"
    "        X16\n"
    "        LDX #$4000\n"
    "wait:   DEX\n"
    "        BNE wait\n"
    "        SEP #$10\n"
    "        X8\n"
    "        LDA $33\n"
    "        STA $35\n"
    "        RTS\n"
    "        ORG $00:8200\n"
    "nmi:    INC $33\n"
    "        RTI\n";

TEST(SnesNesting, AnInterruptArrivingInsideARoutineIsTheGuestsOwn) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kInterrupt));
    auto slow = machine.bindRoutine<void()>(0x0081A0, kPlain);
    bool once = false;
    machine.registerEscapes(escapes(GuestEscape{.key = "tick", .at = 0x008180, .handler = [&](Vm& m, std::uint32_t) {
                                                    if (!once) {
                                                        once = true;
                                                        slow();
                                                        m.escapes()["tick"].armed(false);
                                                    }
                                                }}));

    stepParked(machine, 6);
    ASSERT_TRUE(once);
    EXPECT_GT(byteAt(machine, 0x7E0035), byteAt(machine, 0x7E0034));  // the guest's own NMIs ran, inside the call
}

// ── Whose stack the frame goes on ───────────────────────────────────────────────────────────────

// The guest seats its own stack at $1F00, well below the engine's scratch top, and clears both, so which
// one a call pushed on is a question the bytes answer.
constexpr std::string_view kStack =
    "        REP #$10\n"
    "        X16\n"
    "        LDX #$1F00\n"
    "        TXS\n"
    "        SEP #$10\n"
    "        X8\n"
    "        STZ !$1EFF\n"
    "        STZ !$1F00\n"
    "        STZ !$1FFE\n"
    "        STZ !$1FFF\n"
    "        LDA #$01\n"
    "        STA $36\n"
    "done:   BRA done\n"
    "        ORG $00:8180\n"
    "        A8\n"
    "decode: LDA #$C7\n"
    "        STA $37\n"
    "        RTS\n";

TEST(SnesNesting, ACallOnAParkedBootedCartridgeGoesOnTheGuestsOwnStack) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kStack));
    stepParked(machine, 1);
    ASSERT_EQ(byteAt(machine, 0x7E0036), 0x01);  // the guest ran and seated its own stack

    auto decode = machine.bindRoutine<void()>(0x008180, kPlain);
    decode();
    EXPECT_EQ(byteAt(machine, 0x7E0037), 0xC7);  // the routine's answer
    EXPECT_EQ(byteAt(machine, 0x7E1F00), 0x81);  // the landing's high byte, pushed at the guest's own $1F00
    EXPECT_EQ(byteAt(machine, 0x7E1FFF), 0x00);  // and nothing on the engine's scratch stack
    EXPECT_EQ(byteAt(machine, 0x7E1FFE), 0x00);
}

// Never booted, the machine stands at its reset vector in emulation mode, where the chip keeps the stack in
// page one: the landing goes on the engine's scratch top there, and the guest's own stack is untouched.
TEST(SnesNesting, ACallOnACartridgeThatHasNeverBootedGoesOnTheScratchStack) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kStack));
    const std::uint8_t guestBefore = byteAt(machine, 0x7E1F00);

    auto decode = machine.bindRoutine<void()>(0x008180, kPlain);
    decode();
    EXPECT_EQ(byteAt(machine, 0x7E0037), 0xC7);
    EXPECT_EQ(byteAt(machine, 0x7E01FF), 0x7F);  // $8000 - 1, the reset vector's landing, at page one's top
    EXPECT_EQ(byteAt(machine, 0x7E01FE), 0xFF);
    EXPECT_EQ(byteAt(machine, 0x7E1F00), guestBefore);
}

// A routine does not have to be in the cartridge: code a game copies into work RAM binds and runs where it
// is, through an address in the bank a JSR would reach it from.
TEST(SnesNesting, ARoutineLivingInWorkRamIsCallableToo) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kStack));
    stepParked(machine, 1);

    const std::vector<std::uint8_t> code{0xA9, 0xC7, 0x85, 0x38, 0x60};  // LDA #$C7 / STA $38 / RTS
    machine.write(MemoryRegion{.at = 0x7E0200, .size = static_cast<std::uint32_t>(code.size())}, code);
    machine.write(MemoryRegion{.at = 0x7E0038, .size = 1}, std::vector<std::uint8_t>{0x00});

    auto inRam = machine.bindRoutine<void()>(0x000200, kPlain);
    inRam();
    EXPECT_EQ(byteAt(machine, 0x7E0038), 0xC7);
}

// ── Which thread may call ───────────────────────────────────────────────────────────────────────

TEST(SnesNesting, AMachineRunningOnItsOwnThreadRefusesACallFromAnywhereElse) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kStack));
    auto decode = machine.bindRoutine<void()>(0x008180, kPlain);

    machine.run();  // a thread of its own
    EXPECT_THROW(decode(), std::logic_error);
    machine.stop();

    decode();  // and once it is parked, the caller's thread is the only one there is
    EXPECT_EQ(byteAt(machine, 0x7E0037), 0xC7);
}

// ── A native answer built from the guest's own routine ──────────────────────────────────────────

constexpr std::string_view kDamage =
    "        LDA #$07\n"
    "        STA $39\n"           // the seed the guest's own generator keeps
    "        LDY #$04\n"
    "round:  LDA #$05\n"
    "        LDX #$03\n"
    "        JSR !rule\n"
    "        STA $3A\n"
    "        DEY\n"
    "        BNE round\n"
    "done:   BRA done\n"
    "        ORG $00:8180\n"
    "rule:   STX $3B\n"           // the cartridge's own rule, which the replacement answers instead of
    "        CLC\n"
    "        ADC $3B\n"
    "        RTS\n"
    "        ORG $00:81A0\n"
    "random: LDA $39\n"           // the cartridge's own generator, which the replacement calls
    "        ASL A\n"
    "        INC A\n"
    "        STA $39\n"
    "        RTS\n";

TEST(SnesNesting, AReplacementMayAnswerWithTheCartridgesOwnRoutineInsideIt) {
    Vm::SNES machine;
    machine.hostRom(cartridge(kDamage));
    auto random = machine.bindRoutine<std::uint8_t()>(0x0081A0, RoutineBinding{.output = snes::A, .isa = Isa::Wdc65816});

    std::vector<std::uint8_t> rolls;
    machine.registerEscapes(escapes(GuestEscape{
        .key      = "damage",
        .at       = 0x008180,
        .replaces = routine(RoutineBinding{.inputs = {snes::A, snes::X}, .output = snes::A},
                            [&](std::uint8_t attack, std::uint16_t defense) -> std::uint8_t {
                                const std::uint8_t roll = random();
                                rolls.push_back(roll);
                                return static_cast<std::uint8_t>(attack * defense + roll);
                            })}));

    stepParked(machine, 1);
    EXPECT_EQ(rolls, (std::vector<std::uint8_t>{15, 31, 63, 127}));  // the guest's own generator ran
    EXPECT_EQ(byteAt(machine, 0x7E0039), 127);                        // its seed advanced four times
    EXPECT_EQ(byteAt(machine, 0x7E003A), static_cast<std::uint8_t>(5 * 3 + 127));  // the guest read the answer
    EXPECT_EQ(byteAt(machine, 0x7E003B), 0x00);                       // and the rule's own body never ran
}

// ── What a call leaves installed ────────────────────────────────────────────────────────────────

TEST(SnesNesting, ACallInstallsNoWatcher) {
    SnesBackend backend;
    backend.loadRom(cartridge(kStack));
    backend.bootHostedRom();
    backend.setEscapeSink([](std::uint32_t) {});
    backend.setWatchSink([](std::uint32_t, std::uint32_t, vm::VmBackend::AccessKind, std::uint8_t) {
        return AccessVerdict::proceed();
    });
    backend.callInContext(0x008180, {}, CallStack::Guest, 100, {});
    EXPECT_FALSE(SnesBackendTestAccess::instructionWatcherInstalled(backend));
    EXPECT_FALSE(SnesBackendTestAccess::accessWatcherInstalled(backend));
}

}  // namespace
}  // namespace retropp
