// The SNES core's routines: bytes placed into an image the core writes, each at the address its source
// named or at the first gap that holds it; a call in a frame of the engine's own; the machine's own time
// passing between calls; and a call into code the machine already holds, in the guest's own context.
//
// What a routine has to get right on this console, and the cases below pin one at a time: the frame a
// placed routine begins in; a source with ORG lands there and one without lands where it fits, its
// labels resolving for that address; two routines never share a byte; the image grows a bank when a
// routine asks for one; an address no image byte is at is refused; a reset keeps what was placed; the
// clock moves between calls and the file does not; a call in the guest's context lands at an instruction
// boundary, uses the stack it was told to, and puts the file back whether the routine returned or the
// guard tripped; a routine an RTL leaves is entered as a JSL would enter it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/snes.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"
#include "src/vm/vm_backend.h"

namespace retropp {
namespace {

using vm::AssembledRoutine;
using vm::CallStack;
using vm::ResidentRegister;
using vm::SnesBackend;
using vm::SnesBackendTestAccess;

constexpr auto id = [](snes::Reg r) { return static_cast<std::uint16_t>(r); };

// A one-bank LoROM cartridge whose reset code is `source` — snes_backend_run_test's fixture.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(range.start - 0x8000u));
    }
    return rom;
}

// Place `source` and answer where it landed.
std::uint32_t place(SnesBackend& backend, std::string_view source) {
    const AssembledRoutine assembled = backend.assemble(source);
    return backend.placeRoutine(assembled.bytes, assembled.origin);
}

// Call the routine at `entry` in the engine's frame with A and X preset, and answer the A it left.
std::uint64_t call(SnesBackend& backend, std::uint32_t entry, std::uint8_t a, std::uint16_t x) {
    backend.beginCall(entry);
    backend.writeRegister(id(snes::Reg::A), a, 1);
    backend.writeRegister(id(snes::Reg::X), x, 2);
    backend.run();
    return backend.readRegister(id(snes::Reg::A));
}

// A booted cartridge that idles at reset, with a routine at $00:8100 that counts its calls in work-RAM
// cell $20 and one at $00:8110 that returns across banks.
constexpr std::string_view kIdleWithRoutines =
    "        ORG $00:8000\n"
    "        EMULATION\n"
    "        CLC\n"
    "        XCE\n"
    "        SEP #$30\n"
    "idle:   BRA idle\n"
    "        ORG $00:8100\n"
    "        A8\n"
    "count:  INC $20\n"
    "        RTS\n"
    "        ORG $00:8110\n"
    "far:    RTL\n"
    "        ORG $00:8120\n"
    "spin:   BRA spin\n";

TEST(SnesBackendRoutines, TheFrameIsNativeEightBitWithTheStackAtTheTop) {
    // The routine writes the file it began in to work RAM: P, then S and D through 16-bit pulls, then
    // DB, then the two presets, and leaves a value in A.
    SnesBackend backend;
    const std::uint32_t entry = place(backend,
                                      "        A8\n"
                                      "        X8\n"
                                      "        PHP\n"
                                      "        PLA\n"
                                      "        STA $10\n"
                                      "        REP #$20\n"
                                      "        TSC\n"
                                      "        STA $12\n"
                                      "        PHD\n"
                                      "        PLA\n"
                                      "        STA $14\n"
                                      "        SEP #$20\n"
                                      "        PHB\n"
                                      "        PLA\n"
                                      "        STA $16\n"
                                      "        LDA $18\n"
                                      "        STA $1A\n"
                                      "        STX $1B\n"
                                      "        LDA #$77\n"
                                      "        RTS\n");
    backend.writeMemory(0x7E0018, 0x00, 1);
    backend.beginCall(entry);
    backend.writeRegister(id(snes::Reg::A), 0x03, 1);
    backend.writeRegister(id(snes::Reg::X), 0x05, 2);
    backend.writeMemory(0x7E0018, 0x42, 1);
    backend.run();

    const auto& wram = SnesBackendTestAccess::state(backend).wram;
    EXPECT_EQ(wram[0x10], 0x34);              // native, M X I set, D clear
    EXPECT_EQ(wram[0x12] | wram[0x13] << 8, 0x1FFD);  // $1FFF, less the two bytes of the landing
    EXPECT_EQ(wram[0x14] | wram[0x15] << 8, 0x0000);
    EXPECT_EQ(wram[0x16], 0x00);
    EXPECT_EQ(wram[0x1A], 0x42);              // a memory input written live
    EXPECT_EQ(wram[0x1B], 0x05);              // the X preset, in the 8-bit index mode the frame sets
    EXPECT_EQ(backend.readRegister(id(snes::Reg::A)), 0x77u);
    EXPECT_EQ(backend.readRegister(id(snes::Reg::S)), 0x1FFFu);  // the RTS took the landing back off
    EXPECT_FALSE(SnesBackendTestAccess::state(backend).cpu.e);
}

TEST(SnesBackendRoutines, ASourceWithOrgLandsThereAndOneWithoutLandsWhereItFits) {
    SnesBackend backend;
    EXPECT_EQ(place(backend, "        ORG $00:8400\n        RTS\n"), 0x008400u);
    EXPECT_EQ(backend.readMemory(0x008400, 1), 0x60u);  // RTS

    // No ORG: the first gap from $00:8000, and a label inside it resolves for that address — the JMP's
    // operand is where `done` landed, not where it would be at $000000.
    const AssembledRoutine hop = backend.assemble(
        "        JMP !done\n"
        "        NOP\n"
        "done:   RTS\n");
    ASSERT_TRUE(hop.origin.has_value());
    EXPECT_EQ(*hop.origin, 0x008000u);
    EXPECT_EQ(hop.bytes[1] | hop.bytes[2] << 8, 0x8004);
    EXPECT_EQ(hop.labels.at("done"), 4u);
    EXPECT_EQ(backend.placeRoutine(hop.bytes, hop.origin), 0x008000u);

    // Raw bytes with no origin take the next gap after it, and a second ORG-less source the gap after
    // that — never the routine at $8400.
    const std::array<std::uint8_t, 3> raw{0xEA, 0xEA, 0x60};
    EXPECT_EQ(backend.placeRoutine(raw, std::nullopt), 0x008005u);
    EXPECT_EQ(place(backend, "        RTS\n"), 0x008008u);
    EXPECT_EQ(backend.readMemory(0x008400, 1), 0x60u);
}

TEST(SnesBackendRoutines, TwoRoutinesNeverShareAByte) {
    SnesBackend backend;
    place(backend, "        ORG $00:8400\n        NOP\n        NOP\n        RTS\n");
    EXPECT_THROW(place(backend, "        ORG $00:8402\n        RTS\n"), std::invalid_argument);
    EXPECT_THROW(place(backend, "        ORG $00:7FC0\n        RTS\n"), std::invalid_argument);  // the header
    EXPECT_THROW(place(backend, "        ORG $00:FFB0\n        RTS\n"), std::invalid_argument);  // the idle loop
    EXPECT_THROW(place(backend, "        ORG $00:FFAF\n        NOP\n        RTS\n"), std::invalid_argument);

    // A source with no ORG never collides: a routine wide enough to reach $8400 from $8000 is laid
    // out past it instead.
    std::string wide;
    for (int i = 0; i < 0x401; ++i) wide += "        NOP\n";
    wide += "        RTS\n";
    EXPECT_EQ(place(backend, wide), 0x008403u);
}

TEST(SnesBackendRoutines, TheImageGrowsABankForARoutineThatAsksForOne) {
    SnesBackend backend;
    place(backend, "        ORG $00:8000\n        NOP\n        RTS\n");
    EXPECT_EQ(place(backend, "        ORG $01:8000\n        INX\n        RTS\n"), 0x018000u);
    EXPECT_EQ(backend.readMemory(0x018000, 1), 0xE8u);  // INX: its own bank, not a mirror of the first
    EXPECT_EQ(backend.readMemory(0x008000, 1), 0xEAu);
    EXPECT_TRUE(snaggletooth::parseCartridgeHeader(
                    std::span<const std::uint8_t>(SnesBackendTestAccess::image(backend)))
                    ->checksumAgrees);
}

TEST(SnesBackendRoutines, AnAddressNoImageByteIsAtIsRefused) {
    SnesBackend backend;
    EXPECT_THROW(place(backend, "        ORG $00:2100\n        RTS\n"), std::invalid_argument);  // a register page
    EXPECT_THROW(place(backend, "        ORG $7E:1000\n        RTS\n"), std::invalid_argument);  // work RAM
    EXPECT_THROW((void)backend.assemble("        LDA #1\n"), std::runtime_error);  // a width nothing said
    EXPECT_THROW((void)backend.assemble("        FROB\n"), std::runtime_error);    // not an instruction
}

TEST(SnesBackendRoutines, AResetKeepsPlacedRoutines) {
    SnesBackend backend;
    const std::uint32_t entry = place(backend, "        A8\n        INC $30\n        LDA $30\n        RTS\n");
    EXPECT_EQ(call(backend, entry, 0, 0), 1u);
    EXPECT_EQ(call(backend, entry, 0, 0), 2u);
    backend.reset();
    EXPECT_EQ(backend.readMemory(entry, 1), 0xE6u);  // INC dp, still there
    EXPECT_EQ(call(backend, entry, 0, 0), 1u);       // work RAM is power-on again
}

TEST(SnesBackendRoutines, TheClockMovesBetweenCallsAndTheFileDoesNot) {
    SnesBackend backend;
    const std::uint32_t entry = place(backend, "        RTS\n");
    call(backend, entry, 0x11, 0x2222);
    const snaggletooth::SnesState& state  = SnesBackendTestAccess::state(backend);
    const snaggletooth::Cpu65816State file = state.cpu;
    const std::uint64_t master = state.master;

    backend.advanceClock(10'000);
    EXPECT_GE(state.master, master + 10'000);
    EXPECT_EQ(state.cpu, file);
    EXPECT_EQ(call(backend, entry, 0x33, 0), 0x33u);  // the next call is not refused

    SnesBackend empty;
    EXPECT_THROW(empty.advanceClock(1), std::logic_error);
}

TEST(SnesBackendRoutines, AHostedCartridgeAndARoutineImageRefuseEachOther) {
    SnesBackend hosted;
    hosted.loadRom(cartridgeFrom(kIdleWithRoutines));
    EXPECT_THROW(hosted.placeRoutine(std::array<std::uint8_t, 1>{0x60}, std::nullopt), std::logic_error);
    EXPECT_THROW(hosted.advanceClock(1), std::logic_error);

    SnesBackend routines;
    place(routines, "        RTS\n");
    EXPECT_THROW(routines.loadRom(cartridgeFrom(kIdleWithRoutines)), std::logic_error);
}

TEST(SnesBackendRoutines, ACallInTheGuestsContextUsesItsStackAndPutsTheFileBack) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdleWithRoutines));
    backend.bootHostedRom();
    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    const snaggletooth::Cpu65816State before = state.cpu;

    std::uint64_t seen = 0;
    backend.callInContext(0x008100, {}, CallStack::Guest, 1000,
                          [&] { seen = backend.readRegister(id(snes::Reg::PC)); });
    EXPECT_EQ(state.wram[0x20], 1);
    EXPECT_EQ(seen, before.pc);  // read at the landing, before the file went back
    EXPECT_EQ(state.cpu.pc, before.pc);
    EXPECT_EQ(state.cpu.s, before.s);
    EXPECT_EQ(state.cpu.a, before.a);
    EXPECT_EQ(state.cpu.p, before.p);

    // The scratch stack: the landing is pushed at the engine's own top — page one's top for a guest in
    // emulation mode, where the chip keeps the stack in page one, and $1FFF otherwise.
    const std::uint16_t scratch = before.e ? 0x01FF : 0x1FFF;
    backend.writeMemory(0x7E0000 + scratch - 1, 0x00, 2);
    backend.callInContext(0x008100, {}, CallStack::Scratch, 1000, {});
    EXPECT_EQ(state.wram[0x20], 2);
    EXPECT_EQ(state.wram[scratch - 1] | state.wram[scratch] << 8, before.pc - 1);
    EXPECT_EQ(state.cpu.s, before.s);

    // A preset over the guest's file reaches the routine, and the guest's own value comes back.
    const std::array<ResidentRegister, 1> presets{ResidentRegister{id(snes::Reg::A), 0x5A}};
    std::uint64_t a = 0;
    backend.callInContext(0x008100, presets, CallStack::Guest, 1000,
                          [&] { a = backend.readRegister(id(snes::Reg::A)); });
    EXPECT_EQ(a, 0x5Au);
    EXPECT_EQ(state.cpu.a, before.a);
}

TEST(SnesBackendRoutines, ACallIntoARunningCartridgeLandsAtAnInstructionBoundary) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdleWithRoutines));
    backend.bootHostedRom();
    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    // A cycle budget stops wherever the cycle fell — inside the idle branch more often than not.
    backend.runForCycles(1'001);
    const bool midInstruction = state.cpu.tcu != 0;
    backend.callInContext(0x008100, {}, CallStack::Guest, 1000, {});
    EXPECT_EQ(state.wram[0x20], 1);
    EXPECT_EQ(state.cpu.tcu, 0);
    EXPECT_TRUE(midInstruction || state.cpu.pc == 0x8005 || state.cpu.pc == 0x8007);
    backend.runForCycles(1'000);  // and it carries on
    EXPECT_EQ(state.wram[0x20], 1);
}

TEST(SnesBackendRoutines, ATrippedGuardStillPutsTheFileBack) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdleWithRoutines));
    backend.bootHostedRom();
    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    const snaggletooth::Cpu65816State before = state.cpu;
    const std::uint64_t master = state.master;
    bool read = false;
    backend.callInContext(0x008120, {}, CallStack::Guest, 50, [&] { read = true; });  // spins forever
    EXPECT_TRUE(read);
    EXPECT_GT(state.master, master);
    EXPECT_EQ(state.cpu.pc, before.pc);
    EXPECT_EQ(state.cpu.s, before.s);
}

TEST(SnesBackendRoutines, AnRtlEntryIsEnteredAsAJslWouldEnterIt) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdleWithRoutines));
    backend.bootHostedRom();
    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    const std::uint16_t s = state.cpu.s;
    backend.writeMemory(0x7E0000 + (s - 2), 0xAA, 1);

    // Three bytes pushed — the bank, then the return address — and the RTL takes all three back off.
    backend.callInContext(snes::rtl(0x008110), {}, CallStack::Guest, 100, {});
    EXPECT_EQ(state.cpu.s, s);
    EXPECT_EQ(state.wram[s], state.cpu.pbr);
    EXPECT_EQ(state.wram[s - 1], (state.cpu.pc - 1) >> 8);
    EXPECT_EQ(state.wram[s - 2], (state.cpu.pc - 1) & 0xFF);

    // A near entry pushes two, and the byte below them is not touched.
    backend.writeMemory(0x7E0000 + (s - 2), 0xAA, 1);
    backend.callInContext(0x008100, {}, CallStack::Guest, 100, {});
    EXPECT_EQ(state.wram[s - 2], 0xAA);

    // The bit is code's alone: with a tagged space it names nothing.
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::rtl(snes::videoRam(0)), .size = 1}));
    EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = snes::rtl(0x008110), .size = 1}));
}

TEST(SnesBackendRoutines, TheRegisterIdsHaveTheChipsWidths) {
    SnesBackend backend;
    for (const snes::Reg r : {snes::Reg::A, snes::Reg::B, snes::Reg::P, snes::Reg::DB, snes::Reg::PB}) {
        EXPECT_EQ(backend.registerWidthBytes(id(r)), 1);
    }
    for (const snes::Reg r : {snes::Reg::C, snes::Reg::X, snes::Reg::Y, snes::Reg::D, snes::Reg::S, snes::Reg::PC}) {
        EXPECT_EQ(backend.registerWidthBytes(id(r)), 2);
    }
    EXPECT_EQ(backend.registerWidthBytes(id(snes::Reg::PC) + 1), 0);

    // A and B are the accumulator's halves and C the whole of it.
    const std::uint32_t entry = place(backend, "        RTS\n");
    backend.beginCall(entry);
    backend.writeRegister(id(snes::Reg::C), 0x1234, 2);
    backend.writeRegister(id(snes::Reg::B), 0x56, 1);
    backend.run();
    EXPECT_EQ(backend.readRegister(id(snes::Reg::A)), 0x34u);
    EXPECT_EQ(backend.readRegister(id(snes::Reg::B)), 0x56u);
    EXPECT_EQ(backend.readRegister(id(snes::Reg::C)), 0x5634u);
}

}  // namespace
}  // namespace retropp
