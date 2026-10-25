// Calling the machine's own routines on the real core: what the interrupted instruction finds when
// it resumes, what the call costs the guest, and what happens when a routine takes an interrupt,
// runs away, or lives in a bank that is not mapped.
//
// The device-free suite (guest_nesting_test.cpp) pins the surface on a machine with no CPU. These
// are the claims only a CPU can answer. Every cartridge here is authored by these tests, byte for
// byte (tests/authored_cartridge.h) with its code assembled by the engine's own SM83 assembler — an
// authored image is the only one with ground truth. Running machines are stepped deterministically
// through the inline seam (src/vm/vm_testing.h); the one threaded case asserts a refusal, never a
// wall-clock ratio.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/memory_region.h"
#include "retropp/vm.h"
#include "src/vm/gameboy/sameboy_backend.h"
#include "src/vm/gameboy/sameboy_machine.h"
#include "src/vm/vm_backend.h"
#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

using vm::CallStack;
using vm::ConsoleModel;
using vm::Registers;
using vm::ResidentRegister;
using vm::SameBoyBackend;
using vm::SameBoyMachine;
using vm::VmTestAccess;

// One piece of a cartridge: source assembled and laid down at a physical offset in the image.
struct Block {
    std::size_t      at;
    std::string_view source;
};

// A cartridge whose entry jumps to $0150, with a bare reti on the VBlank vector unless a block
// covers it, and each block's code assembled and placed where it says.
std::vector<std::uint8_t> cartridge(Vm& assembler, const std::vector<Block>& blocks,
                                    std::size_t bytes      = testing::kSmallestCartridge,
                                    std::uint8_t cartridgeType = testing::kRomOnly) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(bytes, cartridgeType);
    rom[0x143] = 0x80;  // a CGB-flagged image
    rom[0x040] = 0xD9;  // VBlank vector: reti
    rom[0x100] = 0x00;
    rom[0x101] = 0xC3;
    rom[0x102] = 0x50;
    rom[0x103] = 0x01;  // jp $0150
    for (const Block& b : blocks) {
        const std::vector<std::uint8_t> code = assembler.assemble(std::string(b.source));
        std::copy(code.begin(), code.end(), rom.begin() + static_cast<std::ptrdiff_t>(b.at));
    }
    return rom;
}

// What these tests observe: the high-RAM cells the guests write, and two windows of work RAM — one
// around the stack a guest chooses for itself, one around the engine's own scratch top.
struct Places {
    MemoryRegion hram;
    MemoryRegion wram;
    MemoryRegion guestStack;
    MemoryRegion scratchStack;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(
        regions(region(&Places::hram, MemoryRegion{.at = 0xFF80, .size = 1, .count = 16}, "hram"),
                region(&Places::wram, MemoryRegion{.at = 0xC000, .size = 1, .count = 64}, "wram"),
                region(&Places::guestStack, MemoryRegion{.at = 0xDFE0, .size = 1, .count = 16},
                       "guest stack"),
                region(&Places::scratchStack, MemoryRegion{.at = 0xDFF0, .size = 1, .count = 12},
                       "scratch stack")));
}

std::uint8_t hram(Vm& machine, const RegionMapId<Places>& places, std::uint32_t index) {
    return machine.read(places, &Places::hram, index).at(0);
}

std::uint8_t byteAt(Vm& machine, std::uint32_t address) {
    return machine.read(MemoryRegion{.at = address, .size = 1}).at(0);
}

// ── The interrupted instruction finds what it left ──────────────────────────────────────────────

// A hundred rounds, each adding three to a checksum the guest keeps in C across a call that is
// escaped — and, inside that escape, a routine of the guest's own that sets every register to $FF.
// The loop counter is a register too, so a register file that did not come back would not even
// finish counting.
constexpr std::string_view kChecksumMain = R"(
    ld b, $64           ; a hundred rounds
    xor a
    ld c, a
    ldh [$FF80], a
loop:
    call $0400
    ld a, c
    add a, $03
    ld c, a
    dec b
    jr nz, loop
    ld a, c
    ldh [$FF80], a      ; 300 & $FF = 44, and only if C survived every round
done:
    jr done
)";

// The escaped instruction is the first one here: a load of a literal, so the operand the CPU reads
// after the escape is the one that was always there.
constexpr std::string_view kEscapedBlock = R"(
    ld a, $2A
    ldh [$FF82], a
    ret
)";

// The guest's own routine, called from inside the escape: it leaves nothing as it found it.
constexpr std::string_view kClobberRoutine = R"(
    ld a, $FF
    ld b, a
    ld c, a
    ld d, a
    ld e, a
    ld h, a
    ld l, a
    ret
)";

std::vector<std::uint8_t> checksumCartridge(Vm& assembler) {
    return cartridge(assembler, {Block{.at = 0x0150, .source = kChecksumMain},
                                 Block{.at = 0x0400, .source = kEscapedBlock},
                                 Block{.at = 0x0500, .source = kClobberRoutine}});
}

std::uint16_t id(gb::Reg reg) { return static_cast<std::uint16_t>(reg); }

TEST(SameBoyNesting, TheInterruptedFrameComesBackExactlyAsItWas) {
    Vm::GBC machine;
    machine.hostRom(checksumCartridge(machine));
    const RegionMapId<Places> places = declarePlaces(machine);

    auto clobber = machine.bindRoutine<void()>(0x0500, RoutineBinding{});

    int fired = 0;
    machine.registerEscapes(escapes(GuestEscape{
        .key = "round", .at = 0x0400, .handler = [&](Vm&, std::uint32_t) {
            ++fired;
            clobber();
        }}));

    VmTestAccess::runInline(machine);
    for (int i = 0; i < 4; ++i) {
        VmTestAccess::stepOnce(machine);
    }

    EXPECT_EQ(fired, 100);                          // the escape fired every round
    EXPECT_EQ(hram(machine, places, 0), 44u);       // and C carried the checksum through all of them
    EXPECT_EQ(hram(machine, places, 2), 0x2A);      // the escaped load kept its own operand
    machine.stop();
}

// ── What the call costs the guest ───────────────────────────────────────────────────────────────

// Measured where cycles are reported: a machine of placed routines, called through the resident
// path, which runs its own GB_run loop and hands back what it spent. The routine called in the
// guest's context reports nothing of its own — its cycles land in that loop's tally, which is the
// whole point — so the difference between the two runs IS the cost.
//
// A DMG machine on purpose: a write to SCY is the one that reads the new value mid-write there, so
// its cost depends on the fetch cycles the interrupted instruction still has latched. On a machine
// that had spent them, this write would advance a wildly different number.
constexpr std::string_view kOuterRoutine = R"(
    ld b, $04
round:
    ldh [$FF42], a      ; SCY — escaped here, four times
    dec b
    jr nz, round
    ret
)";

constexpr std::string_view kAnswerRoutine = R"(
    ld a, $FF
    ret
)";

TEST(SameBoyNesting, ACallInTheGuestsContextCostsItsOwnInstructionsPlusOneFetch) {
    SameBoyBackend backend(ConsoleModel::GameBoy);

    const std::vector<std::uint8_t> outer  = backend.assemble(std::string(kOuterRoutine)).bytes;
    const std::vector<std::uint8_t> answer = backend.assemble(std::string(kAnswerRoutine)).bytes;
    const std::array<DriverImage, 2> images{
        DriverImage{.bytes = outer, .base = 0x1000},
        DriverImage{.bytes = answer, .base = 0x1100}};
    backend.configureResidentImage(images, Mapper{}, 0);

    const std::array<ResidentRegister, 1> presets{
        ResidentRegister{.registerId = static_cast<std::uint16_t>(gb::Reg::A), .value = 0x5C}};

    const std::uint64_t control = backend.callResident(0x1000, presets, 100'000);

    backend.setEscapeSink([&](std::uint32_t) {
        backend.callInContext(0x1100, {}, CallStack::Guest, 1'000, {});
    });
    backend.armEscape(0x1002, /*replacesRoutine=*/false);  // the SCY write inside the loop
    const std::uint64_t escaped = backend.callResident(0x1000, presets, 100'000);

    // ld a,$FF (8) + ret (16) = 24 of the routine's own T-cycles, plus the 4 the landing's fetch
    // costs, four times over.
    EXPECT_EQ(escaped - control, 4u * (24u + 4u));

    // And the write itself carried the guest's own A, not the one the routine left behind.
    EXPECT_EQ(backend.readMemory(0xFF42, 1), 0x5Cu);
}

// A routine that never returns is abandoned at the cap, and the frame still comes back — measured
// where the cap is a parameter rather than the fixed guard the public verb carries, so the claim is
// about what an abandoned call leaves behind rather than about how long it took to give up.
constexpr std::string_view kRunawayRoutine = R"(
    ld a, $FF
    ld b, a
    ld c, a
    ld h, a
    ld l, a
spin:
    jr spin
)";

TEST(SameBoyNesting, ARoutineThatRunsAwayIsAbandonedAndTheFrameStillComesBack) {
    SameBoyBackend backend(ConsoleModel::GameBoyColor);

    const std::vector<std::uint8_t> outer   = backend.assemble(std::string(kOuterRoutine)).bytes;
    const std::vector<std::uint8_t> runaway = backend.assemble(std::string(kRunawayRoutine)).bytes;
    const std::array<DriverImage, 2> images{DriverImage{.bytes = outer, .base = 0x1000},
                                            DriverImage{.bytes = runaway, .base = 0x1100}};
    backend.configureResidentImage(images, Mapper{}, 0);

    int           fires = 0;
    std::uint64_t beforeB = 0, afterB = 0, beforeSp = 0, afterSp = 0;
    backend.setEscapeSink([&](std::uint32_t) {
        const bool first = fires++ == 0;
        if (first) {
            beforeB  = backend.readRegister(id(gb::Reg::B));
            beforeSp = backend.readRegister(id(gb::Reg::SP));
        }
        backend.callInContext(0x1100, {}, CallStack::Guest, /*maxInstructions=*/50, {});
        if (first) {
            afterB  = backend.readRegister(id(gb::Reg::B));
            afterSp = backend.readRegister(id(gb::Reg::SP));
        }
    });
    backend.armEscape(0x1002, /*replacesRoutine=*/false);

    // The outer routine returns, which is the first half of the claim: an abandoned call still ends.
    backend.callResident(0x1000, {}, 100'000);

    EXPECT_EQ(beforeB, 4u);      // the outer routine's own loop counter, before the call
    EXPECT_EQ(afterB, beforeB);  // and back again, though the routine set every register to $FF
    EXPECT_EQ(afterSp, beforeSp);
}

// ── Depth ───────────────────────────────────────────────────────────────────────────────────────

// A handler calls one of the guest's routines; an instruction inside THAT routine escapes, and its
// handler calls another. Each answer reaches its own caller, and the guest's own counting is
// untouched by either.
constexpr std::string_view kDepthMain = R"(
    ld b, $08
    xor a
    ldh [$FF80], a
loop:
    call $0400
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a
    dec b
    jr nz, loop
done:
    jr done
)";

constexpr std::string_view kDepthEscaped = R"(
    nop
    ret
)";

// The first routine: its second instruction is escaped, and it answers with what high RAM holds.
constexpr std::string_view kFirstRoutine = R"(
    nop
    nop                 ; escaped: the handler here calls the second routine
    ldh a, [$FF83]
    add a, a
    ret
)";

// The second: it leaves its own answer where the first one reads it.
constexpr std::string_view kSecondRoutine = R"(
    ld a, $07
    ldh [$FF83], a
    ret
)";

TEST(SameBoyNesting, AHandlersRoutineMayEscapeIntoAHandlerThatCallsAnotherRoutine) {
    Vm::GBC machine;
    machine.hostRom(cartridge(machine, {Block{.at = 0x0150, .source = kDepthMain},
                                        Block{.at = 0x0400, .source = kDepthEscaped},
                                        Block{.at = 0x0500, .source = kFirstRoutine},
                                        Block{.at = 0x0600, .source = kSecondRoutine}}));
    const RegionMapId<Places> places = declarePlaces(machine);

    auto first  = machine.bindRoutine<std::uint8_t()>(
        0x0500, RoutineBinding{.output = gb::A});
    auto second = machine.bindRoutine<void()>(0x0600, RoutineBinding{});

    std::vector<std::uint8_t> answers;
    machine.registerEscapes(escapes(
        GuestEscape{.key = "outermost", .at = 0x0400,
                    .handler = [&](Vm&, std::uint32_t) { answers.push_back(first()); }},
        GuestEscape{.key = "inside the first routine", .at = 0x0501,
                    .handler = [&](Vm&, std::uint32_t) { second(); }}));

    VmTestAccess::runInline(machine);
    for (int i = 0; i < 2; ++i) {
        VmTestAccess::stepOnce(machine);
    }

    ASSERT_EQ(answers.size(), 8u);
    for (const std::uint8_t a : answers) {
        EXPECT_EQ(a, 14u);  // the second routine's 7, doubled by the first
    }
    EXPECT_EQ(hram(machine, places, 0), 8u);  // the guest counted its own rounds regardless
    machine.stop();
}

// ── An interrupt arriving inside a routine ──────────────────────────────────────────────────────

constexpr std::string_view kInterruptMain = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF83], a      ; the interrupt counter
    ldh [$FF0F], a
    ei
loop:
    call $0400
    jr loop
)";

constexpr std::string_view kInterruptVector = R"(
    push af
    ldh a, [$FF83]
    inc a
    ldh [$FF83], a
    pop af
    reti
)";

// Long enough to outlast a frame, and it records the interrupt counter on the way in and out.
constexpr std::string_view kSlowRoutine = R"(
    ldh a, [$FF83]
    ldh [$FF84], a
    ld bc, $2000
wait:
    dec bc
    ld a, b
    or c
    jr nz, wait
    ldh a, [$FF83]
    ldh [$FF85], a
    ret
)";

TEST(SameBoyNesting, AnInterruptArrivingInsideARoutineIsTheGuestsOwn) {
    Vm::GBC machine;
    machine.hostRom(cartridge(machine, {Block{.at = 0x0040, .source = kInterruptVector},
                                        Block{.at = 0x0150, .source = kInterruptMain},
                                        Block{.at = 0x0400, .source = kDepthEscaped},
                                        Block{.at = 0x0500, .source = kSlowRoutine}}));
    const RegionMapId<Places> places = declarePlaces(machine);

    auto slow = machine.bindRoutine<void()>(0x0500, RoutineBinding{});

    bool once = false;
    machine.registerEscapes(escapes(GuestEscape{
        .key = "round", .at = 0x0400, .handler = [&](Vm& m, std::uint32_t) {
            if (!once) {
                once = true;
                slow();
                m.escapes()["round"].armed(false);
            }
        }}));

    VmTestAccess::runInline(machine);
    for (int i = 0; i < 6; ++i) {
        VmTestAccess::stepOnce(machine);
    }

    ASSERT_TRUE(once);
    const std::uint8_t entered = hram(machine, places, 4);
    const std::uint8_t left    = hram(machine, places, 5);
    EXPECT_GT(left, entered);  // the guest's own interrupts ran, inside the call
    machine.stop();
}

// ── Whose stack the frame goes on ───────────────────────────────────────────────────────────────

// The guest seats its own stack well below the engine's scratch top, so which one a call pushed on
// is a question the bytes answer.
// It clears both windows first, because a machine comes up with whatever its RAM happens to hold.
constexpr std::string_view kStackMain = R"(
    ld sp, $DFF0
    xor a
    ld [$DFEE], a
    ld [$DFEF], a
    ld [$DFFA], a
    ld [$DFFB], a
    ld a, $01
    ldh [$FF86], a
done:
    jr done
)";

// Callable with or without a boot: it sits above the window a boot overlay covers.
constexpr std::string_view kDecoderRoutine = R"(
    ld a, $C7
    ld [$C010], a
    ret
)";

std::vector<std::uint8_t> stackCartridge(Vm& assembler) {
    return cartridge(assembler, {Block{.at = 0x0150, .source = kStackMain},
                                 Block{.at = 0x1000, .source = kDecoderRoutine}});
}

TEST(SameBoyNesting, ACallOnAParkedBootedCartridgeGoesOnTheGuestsOwnStack) {
    Vm::GBC machine;
    machine.hostRom(stackCartridge(machine));
    const RegionMapId<Places> places = declarePlaces(machine);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    ASSERT_EQ(hram(machine, places, 6), 0x01);  // the guest ran and seated its own stack

    auto decode = machine.bindRoutine<void()>(0x1000, RoutineBinding{});
    decode();

    EXPECT_EQ(byteAt(machine, 0xC010), 0xC7);   // the routine's answer
    EXPECT_EQ(byteAt(machine, 0xDFEE), 0xFD);   // the landing, pushed below the guest's own $DFF0
    EXPECT_EQ(byteAt(machine, 0xDFEF), 0xFF);
    EXPECT_EQ(byteAt(machine, 0xDFFA), 0x00);   // and nothing on the engine's scratch stack
    EXPECT_EQ(byteAt(machine, 0xDFFB), 0x00);
}

TEST(SameBoyNesting, ACallOnACartridgeThatHasNeverBootedGoesOnTheScratchStack) {
    Vm::GBC machine;
    machine.hostRom(stackCartridge(machine));

    // No run(), so the image has never executed a single instruction of its own — this is reaching
    // content the cartridge stores behind its own code without playing the game to get there. The
    // guest's own stack is therefore whatever the machine came up holding, so it is compared
    // against itself rather than against a value.
    const std::uint8_t guestStackBefore[] = {byteAt(machine, 0xDFEE), byteAt(machine, 0xDFEF)};

    auto decode = machine.bindRoutine<void()>(0x1000, RoutineBinding{});
    decode();

    EXPECT_EQ(byteAt(machine, 0xC010), 0xC7);
    EXPECT_EQ(byteAt(machine, 0xDFFA), 0xFD);   // the landing, on the engine's own scratch stack
    EXPECT_EQ(byteAt(machine, 0xDFFB), 0xFF);
    EXPECT_EQ(byteAt(machine, 0xDFEE), guestStackBefore[0]);  // and nowhere near $DFF0
    EXPECT_EQ(byteAt(machine, 0xDFEF), guestStackBefore[1]);
}

// A routine does not have to be in the cartridge. Games copy code into RAM and call it there, and
// the one answer about what memory a machine has says that address is reachable — so it binds and
// runs like any other.
TEST(SameBoyNesting, ARoutineLivingInWorkRamIsCallableToo) {
    Vm::GBC machine;
    machine.hostRom(stackCartridge(machine));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    // Put the routine there the way a game would: through the ordinary write surface.
    const std::vector<std::uint8_t> code = machine.assemble(R"(
        ld a, $C7
        ld [$C012], a
        ret
    )");
    machine.write(MemoryRegion{.at = 0xC100, .size = static_cast<std::uint32_t>(code.size())}, code);
    // Cleared first, so what is found afterward was put there by the routine rather than by a
    // machine that came up holding it.
    machine.write(MemoryRegion{.at = 0xC012, .size = 1}, std::vector<std::uint8_t>{0x00});

    auto inRam = machine.bindRoutine<void()>(0xC100, RoutineBinding{});
    inRam();

    EXPECT_EQ(byteAt(machine, 0xC012), 0xC7);
}

// ── A routine in a bank ─────────────────────────────────────────────────────────────────────────

constexpr std::string_view kBankMain = R"(
    ld a, $02
    ld [$2000], a       ; select ROM bank 2
done:
    jr done
)";

constexpr std::string_view kBankedRoutine = R"(
    ld a, $B2
    ld [$C020], a
    ret
)";

TEST(SameBoyNesting, ABankedRoutineIsCallableWhileItsOwnBankIsMapped) {
    Vm::GBC machine;
    // 64 KiB, MBC1: four banks, with the same routine bytes in bank 2 and in bank 3.
    std::vector<std::uint8_t> rom =
        cartridge(machine,
                  {Block{.at = 0x0150, .source = kBankMain},
                   Block{.at = 2 * 0x4000, .source = kBankedRoutine},
                   Block{.at = 3 * 0x4000, .source = kBankedRoutine}},
                  0x10000, 0x01);
    machine.hostRom(rom);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    auto inBankTwo   = machine.bindRoutine<void()>(gb::banked(2, 0x4000), RoutineBinding{});
    auto inBankThree = machine.bindRoutine<void()>(gb::banked(3, 0x4000), RoutineBinding{});

    inBankTwo();
    EXPECT_EQ(byteAt(machine, 0xC020), 0xB2);

    try {
        inBankThree();
        FAIL() << "a routine in a bank that is not mapped should not have been callable";
    } catch (const std::logic_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("bank 3"), std::string::npos);
        EXPECT_NE(what.find("bank 2"), std::string::npos);
    }
}

// ── Which thread may call ───────────────────────────────────────────────────────────────────────

TEST(SameBoyNesting, AMachineRunningOnItsOwnThreadRefusesACallFromAnywhereElse) {
    Vm::GBC machine;
    machine.hostRom(checksumCartridge(machine));
    auto clobber = machine.bindRoutine<void()>(0x0500, RoutineBinding{});

    machine.run();  // a thread of its own
    EXPECT_THROW(clobber(), std::logic_error);
    machine.stop();

    clobber();  // and once it is parked, the caller's thread is the only one there is
}

// ── The level-4 shape: a native answer built from the guest's own routine ───────────────────────

constexpr std::string_view kDamageMain = R"(
    ld a, $07
    ldh [$FF87], a      ; the seed the guest's own generator keeps
    ld d, $04
round:
    ld b, $05
    ld c, $03
    call $0400
    ldh [$FF88], a
    dec d
    jr nz, round
done:
    jr done
)";

// The cartridge's own rule, which the replacement answers instead of.
constexpr std::string_view kDamageRule = R"(
    ld a, b
    add a, c
    ret
)";

// The cartridge's own generator, which the replacement calls.
constexpr std::string_view kRandomRoutine = R"(
    ldh a, [$FF87]
    add a, a
    inc a
    ldh [$FF87], a
    ret
)";

TEST(SameBoyNesting, AReplacementMayAnswerWithTheCartridgesOwnRoutineInsideIt) {
    Vm::GBC machine;
    machine.hostRom(cartridge(machine, {Block{.at = 0x0150, .source = kDamageMain},
                                        Block{.at = 0x0400, .source = kDamageRule},
                                        Block{.at = 0x0500, .source = kRandomRoutine}}));
    const RegionMapId<Places> places = declarePlaces(machine);

    auto random = machine.bindRoutine<std::uint8_t()>(0x0500, RoutineBinding{.output = gb::A});

    std::vector<std::uint8_t> rolls;
    machine.registerEscapes(escapes(GuestEscape{
        .key      = "damage",
        .at       = 0x0400,
        .replaces = routine(RoutineBinding{.inputs = {gb::B, gb::C}, .output = gb::A},
                            [&](std::uint8_t attack, std::uint8_t defence) -> std::uint8_t {
                                const std::uint8_t roll = random();
                                rolls.push_back(roll);
                                return static_cast<std::uint8_t>(attack * defence + roll);
                            })}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);

    ASSERT_EQ(rolls.size(), 4u);
    EXPECT_EQ(rolls, (std::vector<std::uint8_t>{15, 31, 63, 127}));  // the guest's own generator ran
    EXPECT_EQ(hram(machine, places, 7), 127u);                       // its seed advanced four times
    EXPECT_EQ(hram(machine, places, 8),
              static_cast<std::uint8_t>(5 * 3 + rolls.back()));  // and the guest read the answer
    machine.stop();
}

// ── Idling puts the guest back where it was ─────────────────────────────────────────────────────

// It leaves a mark the moment it reaches its entry, so a guest that fell back to the entry point
// rather than resuming is not merely counting differently — it says so.
constexpr std::string_view kCountingMain = R"(
    ld a, $AA
    ld [$C030], a       ; the program started from its entry
    xor a
    ldh [$FF89], a
loop:
    ldh a, [$FF89]
    inc a
    ldh [$FF89], a
    jr loop
)";

TEST(SameBoyNesting, IdlingTheClockLeavesABootedCartridgeWhereItWas) {
    Vm::GBC machine;
    machine.hostRom(cartridge(machine, {Block{.at = 0x0150, .source = kCountingMain}}));
    const RegionMapId<Places> places = declarePlaces(machine);

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    const std::uint8_t counted = hram(machine, places, 9);
    ASSERT_GT(counted, 0u);
    ASSERT_EQ(byteAt(machine, 0xC030), 0xAA);  // it has been through its entry, once

    // Rub the mark out, so anything found there afterward was written by a second pass through the
    // entry — which is where a guest parked on the idle landing ends up.
    machine.write(MemoryRegion{.at = 0xC030, .size = 1}, std::vector<std::uint8_t>{0x00});

    machine.advanceTick();  // the divider moves; the guest must not

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(byteAt(machine, 0xC030), 0x00);     // it never went back to its entry
    EXPECT_NE(hram(machine, places, 9), counted);  // and it carried on counting where it left off
}

// ── What an unwatched machine carries ───────────────────────────────────────────────────────────

constexpr std::string_view kThreeNops = R"(
    nop
    nop
    nop
)";

constexpr std::string_view kSpinOnly = R"(
spin:
    jr spin
)";

TEST(SameBoyNesting, ACallInstallsTheHookOnlyForAsLongAsItRuns) {
    Vm::GBC          assembler;
    SameBoyMachine   m(ConsoleModel::GameBoyColor);
    m.loadRom(cartridge(assembler, {Block{.at = 0x0900, .source = kThreeNops},
                                    Block{.at = 0x0910, .source = kSpinOnly}}));
    m.reset();

    EXPECT_FALSE(m.hookInstalled());

    m.setRegisters(Registers{.pc = 0x0900});
    EXPECT_EQ(m.runInContext(0x0902, 8), 2u);  // two nops, then the landing was fetched
    EXPECT_FALSE(m.hookInstalled());

    // And a routine that never reaches its landing stops at the cap instead.
    m.setRegisters(Registers{.pc = 0x0910});
    EXPECT_EQ(m.runInContext(0x0902, 5), 5u);
    EXPECT_FALSE(m.hookInstalled());
}

}  // namespace
}  // namespace retropp
