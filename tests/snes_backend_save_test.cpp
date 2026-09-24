// The cartridge's battery save through the core: what it keeps, refusing a save of the wrong size, a
// read-and-clear changed flag, and a restore that is a load rather than a guest write — the core
// answering `keepsSaveData()`/`saveDataExtension()` and the machine reporting a changed window
// (snes.h:265-274).

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

// One frame of the NTSC console, so the changed-window report (told once per finished frame) has fired.
constexpr std::uint64_t kOneFrame = 357'366u;

// A one-bank LoROM cartridge whose header declares 8 KB of battery save (chipset low nibble $2, save-size
// code $03 = 1 KB << 3) and whose reset code is `source`.
std::vector<std::uint8_t> saveCartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    rom[0x7FC0u + 0x16] = 0x02u;  // chipset: RAM + battery
    rom[0x7FC0u + 0x18] = 0x03u;  // save-size code: 8 KB
    return rom;
}

// A battery cartridge that stores one distinctive byte into the save window and rests.
constexpr std::string_view kSaver =
    "        ORG $00:8000\n"
    "        A8\n"
    "        X8\n"
    "        LDA #$5A\n"
    "        STA $70:0000\n"  // save[0] <- $5A (the LoROM save window)
    "here:   BRA here\n";

// A battery cartridge that writes nothing.
constexpr std::string_view kIdle =
    "        ORG $00:8000\n"
    "here:   BRA here\n";

TEST(SnesBackendSave, TheExtensionIsSrm) {
    SnesBackend backend;
    EXPECT_EQ(backend.saveDataExtension(), "srm");
    EXPECT_TRUE(backend.keepsSaveData());
}

TEST(SnesBackendSave, AWrongSizeSaveIsRefused) {
    SnesBackend backend;
    backend.loadRom(saveCartridgeFrom(kIdle));
    const std::size_t kept = backend.saveDataSize();
    ASSERT_GT(kept, 0u);

    const std::vector<std::uint8_t> tooLong(kept + 1, 0u);
    EXPECT_THROW(backend.writeSaveData(tooLong), std::invalid_argument);

    SnesBackend empty;
    EXPECT_THROW(empty.writeSaveData(std::vector<std::uint8_t>(kept, 0u)), std::invalid_argument);
}

TEST(SnesBackendSave, ASaveRoundTrips) {
    SnesBackend written;
    written.loadRom(saveCartridgeFrom(kSaver));
    written.runForCycles(kOneFrame + 20'000);  // the store executes; a frame reports the change

    const std::vector<std::uint8_t> saved = written.readSaveData();
    ASSERT_FALSE(saved.empty());
    EXPECT_EQ(saved[0], 0x5Au);

    SnesBackend loaded;
    loaded.loadRom(saveCartridgeFrom(kSaver));  // not run: its save is still blank
    loaded.writeSaveData(saved);
    EXPECT_EQ(loaded.readSaveData(), saved);
    EXPECT_EQ(SnesBackendTestAccess::state(loaded).sram, saved);
}

TEST(SnesBackendSave, TheChangedFlagIsReadAndClear) {
    SnesBackend backend;
    backend.loadRom(saveCartridgeFrom(kSaver));
    backend.runForCycles(kOneFrame + 20'000);  // a frame finishes with the store in it

    EXPECT_TRUE(backend.takeSaveDataChanged());
    EXPECT_FALSE(backend.takeSaveDataChanged());  // asking cleared it
}

TEST(SnesBackendSave, ARestoreDoesNotRaiseTheChangedFlag) {
    SnesBackend backend;
    backend.loadRom(saveCartridgeFrom(kIdle));
    const std::vector<std::uint8_t> save(backend.saveDataSize(), 0x7Eu);

    backend.writeSaveData(save);  // an adapter restore, not a guest store
    EXPECT_FALSE(backend.takeSaveDataChanged());
}

}  // namespace
}  // namespace retropp
