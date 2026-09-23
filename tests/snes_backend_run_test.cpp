// The SNES core's lifecycle and determinism: a hosted cartridge boots to power-on, a budget split
// across calls advances the machine exactly as one call would, and a reset returns to power-on while
// the battery save survives it — the core answering `VmBackend` the way `SameBoyBackend` does.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

// A one-bank LoROM cartridge whose reset code is `source`, laid over the canonical header — the
// `snaggletooth_link_test.cpp` pattern. `source` places its first byte at $00:8000, where reset points.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return rom;
}

// The same, with the header patched to declare 8 KB of battery-backed save RAM (chipset low nibble $2,
// save-size code $03 = 1 KB << 3), so a store to the LoROM save window lands and the machine keeps it.
std::vector<std::uint8_t> saveCartridgeFrom(std::string_view source) {
    std::vector<std::uint8_t> rom = cartridgeFrom(source);
    const std::size_t site = 0x7FC0u;
    rom[site + 0x16] = 0x02u;  // chipset: RAM + battery
    rom[site + 0x18] = 0x03u;  // save-size code: 8 KB
    return rom;
}

// A fixture that increments work-RAM cell $00 forever — its counter advances as the machine runs, and
// two machines given the same budget land byte-identical.
constexpr std::string_view kCounter =
    "        ORG $00:8000\n"
    "        A8\n"
    "        X8\n"
    "        LDA #$00\n"
    "        STA $00\n"
    "loop:   INC $00\n"
    "        BRA loop\n";

TEST(SnesBackendRun, HostingACartridgeIsPowerOn) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kCounter));

    EXPECT_TRUE(SnesBackendTestAccess::hosting(backend));
    EXPECT_EQ(SnesBackendTestAccess::state(backend).master, 0u);  // not stepped yet
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0], 0u);  // work RAM cleared at power-on

    SnesBackend empty;
    EXPECT_THROW(empty.loadRom({}), std::invalid_argument);
}

TEST(SnesBackendRun, ASplitBudgetLandsWhereOneCallWould) {
    SnesBackend whole;
    SnesBackend split;
    whole.loadRom(cartridgeFrom(kCounter));
    split.loadRom(cartridgeFrom(kCounter));

    whole.runForCycles(20'000);
    split.runForCycles(7'000);
    split.runForCycles(13'000);

    // SnesState has no operator==, so compare the members a divergence would move: the counters and the
    // work RAM (the fixture's counter lives there). run(a) then run(b) == run(a + b) is the machine's
    // own contract (snes.h).
    const snaggletooth::SnesState& a = SnesBackendTestAccess::state(whole);
    const snaggletooth::SnesState& b = SnesBackendTestAccess::state(split);
    EXPECT_EQ(a.master, b.master);
    EXPECT_EQ(a.consumed, b.consumed);
    EXPECT_EQ(a.wram, b.wram);
}

TEST(SnesBackendRun, TheFixtureCounterAdvances) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kCounter));

    backend.runForCycles(40'000);  // spans many iterations of the increment loop
    EXPECT_NE(SnesBackendTestAccess::state(backend).wram[0], 0u);
}

TEST(SnesBackendRun, ResetReturnsToPowerOnAndKeepsTheSave) {
    // Writes a distinctive save byte and a work-RAM scratch byte, then rests.
    constexpr std::string_view kSaver =
        "        ORG $00:8000\n"
        "        A8\n"
        "        X8\n"
        "        LDA #$5A\n"
        "        STA $70:0000\n"  // save[0] <- $5A (the LoROM save window)
        "        LDA #$99\n"
        "        STA $10\n"       // work-RAM scratch $10 <- $99
        "here:   BRA here\n";

    SnesBackend backend;
    backend.loadRom(saveCartridgeFrom(kSaver));
    backend.runForCycles(20'000);  // the code writes both bytes and rests

    ASSERT_EQ(SnesBackendTestAccess::state(backend).wram[0x10], 0x99u);
    const std::vector<std::uint8_t> saved = backend.readSaveData();
    ASSERT_FALSE(saved.empty());
    EXPECT_EQ(saved[0], 0x5Au);

    backend.reset();

    EXPECT_EQ(SnesBackendTestAccess::state(backend).master, 0u);        // power-on again
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x10], 0u);    // the scratch byte is cleared
    EXPECT_EQ(backend.readSaveData(), saved);                          // the battery survived the reset

    SnesBackend fresh;
    EXPECT_THROW(fresh.reset(), std::logic_error);  // reset before any loadRom
}

}  // namespace
}  // namespace retropp
