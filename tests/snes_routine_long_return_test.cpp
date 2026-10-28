// A placed routine says how it returns. A routine an RTL leaves is placed from its bytes as they are,
// with `.returns = RoutineReturn::Long` on its binding, and the call into it pushes the three-byte landing
// a JSL would, so the RTL returns to it. The default is a routine an RTS leaves. Where the address carries
// the return (a bound routine, an escape's replacement) the binding may not say it as well, and an ISA
// with one return or a routine driven at hardware speed refuses Long at registration.
#include <algorithm>
#include <array>
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
#include "retropp/isa.h"
#include "retropp/snes.h"
#include "retropp/vm.h"

namespace retropp {
namespace {

constexpr std::array<std::uint8_t, 2> kIncrementLong{0x1A, 0x6B};  // INC A ; RTL
constexpr std::array<std::uint8_t, 2> kIncrement{0x1A, 0x60};      // INC A ; RTS
constexpr std::array<std::uint8_t, 1> kRtl{0x6B};                  // RTL
constexpr std::array<std::uint8_t, 1> kRts{0x60};                  // RTS
constexpr std::array<std::uint8_t, 1> kRet{0xC9};                  // RET, the SM83's one return

// SEP #$20 ; LDA [$F3] ; STA $7F:C400 ; RTL — one byte read through the long pointer at $F3-$F5 and
// stored in bank $7F.
constexpr std::array<std::uint8_t, 9> kCopyThroughALongPointer{0xE2, 0x20, 0xA7, 0xF3, 0x8F,
                                                                      0x00, 0xC4, 0x7F, 0x6B};

// The reason a registration was refused, or an empty string when it was accepted.
template <typename Registration>
std::string refusalOf(Registration&& registration) {
    try {
        registration();
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return {};
}

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

// Native mode, 8-bit registers, then INC A and a branch back, forever.
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

TEST(SnesRoutineLongReturn, ARoutineAnRtlLeavesAnswersThroughARegister) {
    Vm::SNES machine;
    auto increment = machine.uploadRoutine<std::uint8_t(std::uint8_t)>(
        kIncrementLong, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816,
                         .returns = RoutineReturn::Long});
    EXPECT_EQ(increment(3), 4);
    EXPECT_EQ(increment(255), 0);
}

// The routine reads its source through a long pointer in the direct page and stores with a long address.
// Power-on RAM is seeded, so the source byte is written before the call and the destination is read only
// after it.
TEST(SnesRoutineLongReturn, TheRequestersShapeReadsThroughALongPointerAndStoresLong) {
    Vm::SNES machine;
    auto copy = machine.uploadRoutine<void(std::uint16_t, std::uint8_t)>(
        kCopyThroughALongPointer,
        {.inputs = {Location::memory(0x0000F3), Location::memory(0x0000F5)},
         .isa = Isa::Wdc65816, .returns = RoutineReturn::Long});
    machine.write(MemoryRegion{.at = 0x7E1234, .size = 1}, std::vector<std::uint8_t>{0x5A});
    copy(0x1234, 0x7E);
    EXPECT_EQ(machine.read(MemoryRegion{.at = 0x7FC400, .size = 1}).at(0), 0x5A);
}

TEST(SnesRoutineLongReturn, TheAsmFormSaysItsReturnTheSameWay) {
    Vm::SNES machine;
    // The path is a string literal and the binding names the ISA: both are what the build scan reads.
    auto increment = machine.registerRoutine<std::uint8_t(std::uint8_t)>(
        "tests/fixtures/routines/snes_increment_long.asm",
        {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816, .returns = RoutineReturn::Long});
    EXPECT_EQ(increment(41), 42);
}

// The routine's frame starts with the stack at $1FFF; the call pushes three bytes and the RTL pulls three.
TEST(SnesRoutineLongReturn, ALongReturnLeavesTheStackWhereTheCallFoundIt) {
    Vm::SNES machine;
    auto stack = machine.uploadRoutine<std::uint16_t()>(
        kRtl, {.output = snes::S, .isa = Isa::Wdc65816, .returns = RoutineReturn::Long});
    EXPECT_EQ(stack(), 0x1FFF);
}

// A register answer is set before the return runs, so the stack pointer after the return is what tells
// the two forms apart: an RTS pulls the two bytes a near call pushed, and nothing else.
TEST(SnesRoutineLongReturn, TheDefaultIsARoutineAnRtsLeaves) {
    Vm::SNES machine;
    auto unsaid = machine.uploadRoutine<std::uint8_t(std::uint8_t)>(
        kIncrement, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816});
    EXPECT_EQ(unsaid(3), 4);
    auto near = machine.uploadRoutine<std::uint8_t(std::uint8_t)>(
        kIncrement, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816,
                     .returns = RoutineReturn::Near});
    EXPECT_EQ(near(3), 4);
    auto stackUnsaid = machine.uploadRoutine<std::uint16_t()>(
        kRts, {.output = snes::S, .isa = Isa::Wdc65816});
    EXPECT_EQ(stackUnsaid(), 0x1FFF);
    auto stackNear = machine.uploadRoutine<std::uint16_t()>(
        kRts, {.output = snes::S, .isa = Isa::Wdc65816, .returns = RoutineReturn::Near});
    EXPECT_EQ(stackNear(), 0x1FFF);
}

TEST(SnesRoutineLongReturn, LongIsRefusedOnAnIsaWithOneReturn) {
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((vm.uploadRoutine<void()>(kRet, {.returns = RoutineReturn::Long})),
                 std::invalid_argument);
    EXPECT_NO_THROW((vm.uploadRoutine<void()>(kRet, {})));

    // A binding for another CPU is refused for its ISA before its return is looked at.
    Vm::SNES machine;
    const std::string reason = refusalOf([&] {
        (void)machine.uploadRoutine<void()>(kRet, {.isa = Isa::Sm83, .returns = RoutineReturn::Long});
    });
    EXPECT_NE(reason.find("RoutineBinding.isa names SM83"), std::string::npos) << reason;
}

// A bound address carries its return in snes::rtl, so the binding may not say it as well.
TEST(SnesRoutineLongReturn, LongIsRefusedOnBindRoutine) {
    Vm::SNES machine;
    (void)machine.uploadRoutine<std::uint8_t(std::uint8_t)>(
        kIncrement, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816});  // at $00:8000
    EXPECT_THROW((machine.bindRoutine<std::uint8_t(std::uint8_t)>(
                     0x008000, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816,
                                .returns = RoutineReturn::Long})),
                 std::invalid_argument);
    auto bound = machine.bindRoutine<std::uint8_t(std::uint8_t)>(
        0x008000, {.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816});
    EXPECT_EQ(bound(3), 4);
}

// An escape's address carries the return of the routine it replaces, so the binding may not say it.
TEST(SnesRoutineLongReturn, LongIsRefusedOnAReplacementBinding) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kLoopSource));
    const std::string reason = refusalOf([&] {
        machine.registerEscapes(escapes(GuestEscape{
            .key      = "loop",
            .at       = kLoop,
            .replaces = routine(RoutineBinding{.output = snes::A, .isa = Isa::Wdc65816,
                                               .returns = RoutineReturn::Long},
                                []() -> std::uint8_t { return 1; })}));
    });
    EXPECT_NE(reason.find("loop: "), std::string::npos) << reason;
}

// A routine driven at hardware speed is started and never returns, so it has no return to name.
TEST(SnesRoutineLongReturn, LongIsRefusedOnADrivenRoutine) {
    Vm::SNES machine;
    EXPECT_THROW((machine.uploadRoutine<void()>(
                     kIncrementLong, {.throttle = Throttle::HardwareSpeed, .isa = Isa::Wdc65816,
                                      .returns = RoutineReturn::Long})),
                 std::invalid_argument);
}

}  // namespace
}  // namespace retropp
