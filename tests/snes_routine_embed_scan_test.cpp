// The register* build scan for a 65816 routine, proven end to end. Because the registerRoutine call
// below names a project-root-relative .asm and a binding whose ISA is Isa::Wdc65816, the build scanned
// this source, ran Snaggletooth's 65816 assembler on that .asm AT BUILD TIME, laid the routine out where
// it fits — the source names no address of its own — and recorded the bytes, their origin and the ISA in
// the routine registry. So findEmbeddedRoutine answers the bytes here with NO runtime .asm read, the
// placement says where they go, and the machine runs them there. This test is that path's paper trail;
// routine_embed_scan_test is the SM83 assembler's.
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "retropp/isa.h"
#include "retropp/routine_registry.h"
#include "retropp/snes.h"
#include "retropp/vm.h"

namespace retropp {
namespace {

// The fixture's text, as the scan assembled it (with the origin it chose said first).
constexpr std::string_view kAverageSource =
    "        ORG $00:8000\n"
    "        A8\n"
    "        X8\n"
    "        STA $00\n"
    "        TXA\n"
    "        CLC\n"
    "        ADC $00\n"
    "        ROR A\n"
    "        RTS\n";

TEST(SnesRoutineEmbedScan, EmbedBakesA65816RoutineWhereItFits) {
    Vm::SNES machine;
    // The path is a string LITERAL and the binding names the ISA — both are what the scan reads.
    auto average = machine.registerRoutine<std::uint8_t(std::uint8_t, std::uint16_t)>(
        "tests/fixtures/routines/snes_average.asm",
        {.inputs = {snes::A, snes::X}, .output = snes::A, .isa = Isa::Wdc65816});

    const std::span<const std::uint8_t> baked =
        detail::findEmbeddedRoutine("tests/fixtures/routines/snes_average.asm");
    ASSERT_FALSE(baked.empty()) << "the scan did not bake the routine into the binary";
    const std::optional<detail::EmbeddedPlacement> placement =
        detail::findEmbeddedRoutinePlacement("tests/fixtures/routines/snes_average.asm");
    ASSERT_TRUE(placement.has_value());
    EXPECT_EQ(placement->isa, Isa::Wdc65816);
    EXPECT_EQ(placement->origin, 0x008000u);  // the first routine of this binary with no ORG of its own

    // The baked bytes are what the assembler produces for the same source at that address.
    const snaggletooth::assembler::Assembly expected =
        snaggletooth::assembler::assembleCpu65816(kAverageSource, "expected.asm");
    ASSERT_TRUE(expected.ok());
    ASSERT_EQ(expected.ranges.size(), 1u);
    EXPECT_EQ(std::vector<std::uint8_t>(baked.begin(), baked.end()), expected.ranges.front().bytes);

    EXPECT_EQ(average(3, 5), 4);
    EXPECT_EQ(average(255, 255), 255);
    EXPECT_EQ(machine.read(MemoryRegion{.at = 0x008000, .size = 1}).at(0), 0x85u);  // STA dp: the bytes are there
}

TEST(SnesRoutineEmbedScan, ABindingForAnotherIsaIsRefused) {
    Vm::SNES machine;
    const std::vector<std::uint8_t> ret{0x60};
    EXPECT_THROW((machine.uploadRoutine<void()>(ret, {.isa = Isa::Sm83})), std::invalid_argument);
    EXPECT_NO_THROW((machine.uploadRoutine<void()>(ret, {.isa = Isa::Wdc65816})));
}

}  // namespace
}  // namespace retropp
