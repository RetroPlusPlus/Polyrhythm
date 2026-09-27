// The SNES core's places: a region names a byte of work RAM, the cartridge image or its save by any bus
// address that reaches it, or a byte of a memory the bus cannot name by the space in its top byte — and
// every region verb reads and writes that byte, wherever the place resolves.
//
// What a place has to get right on this console, and the cases below pin one at a time: every alias of a
// byte is the byte; a run strides through the memory it starts in, not through the addresses after its
// base; a write to the image patches both the machine and the image a rebuild starts from; the save
// window is where each map puts it; a register reads as it stands and takes no write; open bus is not a
// place; a memory ends where it ends.

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
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_address.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

// A one-bank LoROM cartridge whose reset code is `source`, laid over the canonical header —
// snes_backend_run_test's fixture. `source` places its first byte at $00:8000, where reset points.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source, std::size_t banks = 1) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(banks);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return rom;
}

// A header's chipset and save-size bytes set to declare 8 KB of battery-backed save RAM.
void declareSave(std::vector<std::uint8_t>& rom, std::size_t site) {
    rom[site + 0x16] = 0x02u;  // chipset: RAM + battery
    rom[site + 0x18] = 0x03u;  // save-size code: 8 KB
}

// Cartridges whose headers name each map, with an 8 KB save — no program, since a place is read and
// written without running anything. ExHiROM's header sits past the first 4 MB, so its image does too.
std::vector<std::uint8_t> loRomWithSave() {
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    declareSave(rom, 0x7FC0u);
    return rom;
}
std::vector<std::uint8_t> hiRomWithSave() {
    std::vector<std::uint8_t> rom = snaggletooth::examples::hiRomImage(1);
    declareSave(rom, 0xFFC0u);
    return rom;
}
std::vector<std::uint8_t> exHiRomWithSave() {
    std::vector<std::uint8_t> rom(0x410000u, 0u);
    const std::size_t site = 0x40FFC0u;
    for (std::size_t i = 0; i < 21; ++i) rom[site + i] = 'A';
    rom[site + 0x15] = 0x25u;  // the map-mode byte: ExHiROM
    rom[site + 0x1C] = 0x34u;  // a complement and checksum that agree
    rom[site + 0x1D] = 0x12u;
    rom[site + 0x1E] = 0xCBu;
    rom[site + 0x1F] = 0xEDu;
    declareSave(rom, site);
    return rom;
}

std::uint8_t byteAt(SnesBackend& backend, std::uint32_t address) {
    return static_cast<std::uint8_t>(backend.readMemory(address, 1));
}

TEST(SnesBackendPlaces, EveryAliasOfAWorkRamByteIsThatByte) {
    // Stores $5A in work-RAM cell $10 through the direct page, then rests.
    SnesBackend backend;
    backend.loadRom(cartridgeFrom("        ORG $00:8000\n"
                                  "        A8\n"
                                  "        X8\n"
                                  "        LDA #$5A\n"
                                  "        STA $10\n"
                                  "here:   BRA here\n"));
    backend.runForCycles(20'000);

    // The low 8 KB answers in bank $7E and in the low page of every system bank.
    for (const std::uint32_t alias : {0x000010u, 0x7E0010u, 0x3F0010u, 0x800010u, 0xBF0010u}) {
        EXPECT_EQ(byteAt(backend, alias), 0x5Au) << "alias " << alias;
    }

    // A write through any alias lands on the byte, and every other alias reads it.
    backend.writeMemory(0x3F0020u, 0x77u, 1);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x20], 0x77u);
    EXPECT_EQ(byteAt(backend, 0x7E0020u), 0x77u);

    // Work RAM past the mirrored 8 KB has one address in bank $7E, and the upper 64 KB is bank $7F.
    backend.writeMemory(0x7E2345u, 0xC3u, 1);
    backend.writeMemory(0x7F0010u, 0x3Cu, 1);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x2345], 0xC3u);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x10010], 0x3Cu);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x10], 0x5Au);  // the $7F write left bank $7E alone

    // A word reads and writes little-endian, as the 65816 stores one.
    backend.writeMemory(0x7E0030u, 0xBEEFu, 2);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x30], 0xEFu);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x31], 0xBEu);
    EXPECT_EQ(backend.readMemory(0x000030u, 2), 0xBEEFu);
}

TEST(SnesBackendPlaces, AWriteToTheImagePatchesTheMachineAndSurvivesAReset) {
    // Three bytes of the image at $00:9000; the program copies the middle one into work RAM forever,
    // so the machine's own reads show what its copy of the image holds.
    SnesBackend backend;
    backend.loadRom(cartridgeFrom("        ORG $00:8000\n"
                                  "        A8\n"
                                  "        X8\n"
                                  "loop:   LDA !$9001\n"
                                  "        STA $20\n"
                                  "        BRA loop\n"
                                  "        ORG $00:9000\n"
                                  "        DB $11,$22,$33\n"));

    std::array<std::uint8_t, 3> bytes{};
    backend.readRegion(MemoryRegion{.at = 0x009000u, .size = 3}, 0, bytes);
    EXPECT_EQ(bytes, (std::array<std::uint8_t, 3>{0x11, 0x22, 0x33}));
    EXPECT_EQ(byteAt(backend, 0x809001u), 0x22u);  // the image repeats in the upper banks

    const std::array<std::uint8_t, 1> patch{0xAB};
    backend.writeRegion(MemoryRegion{.at = 0x809001u, .size = 1}, 0, patch);
    EXPECT_EQ(byteAt(backend, 0x009001u), 0xABu);
    backend.runForCycles(20'000);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x20], 0xABu);  // the program read the patch

    backend.reset();
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x20], 0x00u);  // power-on again
    EXPECT_EQ(byteAt(backend, 0x009001u), 0xABu);
    backend.runForCycles(20'000);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[0x20], 0xABu);  // the rebuilt machine has it too
}

TEST(SnesBackendPlaces, TheSaveWindowReadsAndWritesWhereEachMapPutsIt) {
    struct Case {
        const char*               name;
        std::vector<std::uint8_t> rom;
        std::uint32_t             save;    // the save's byte 0x10, in the map's first save bank
        std::uint32_t             mirror;  // the same byte through another bank of the window
    };
    const Case cases[] = {
        {.name = "LoROM", .rom = loRomWithSave(), .save = 0x700010u, .mirror = 0xF00010u},
        {.name = "HiROM", .rom = hiRomWithSave(), .save = 0x206010u, .mirror = 0xA06010u},
        {.name = "ExHiROM", .rom = exHiRomWithSave(), .save = 0x806010u, .mirror = 0xA06010u},
    };
    for (const Case& c : cases) {
        SnesBackend backend;
        backend.loadRom(c.rom);
        ASSERT_EQ(backend.saveDataSize(), 0x2000u) << c.name;

        const std::array<std::uint8_t, 2> value{0x5A, 0xA5};
        backend.writeRegion(MemoryRegion{.at = c.save, .size = 2}, 0, value);
        EXPECT_EQ(backend.readSaveData()[0x10], 0x5Au) << c.name;
        EXPECT_EQ(backend.readSaveData()[0x11], 0xA5u) << c.name;
        EXPECT_EQ(backend.readMemory(c.mirror, 2), 0xA55Au) << c.name;
        EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = c.save - 0x10u, .size = 0x2000u})) << c.name;
    }
}

TEST(SnesBackendPlaces, ARunCrossingALoRomBankBoundaryReadsTheImagesNextBytes) {
    // A two-bank LoROM image: the last two bytes of bank $00's window and the first two of bank $01's
    // are image offsets $7FFE-$8001, one after another. The address after $00:FFFF is $01:0000, which is
    // the work-RAM mirror, not the image.
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(2);
    rom[0x7FFE] = 0x01;
    rom[0x7FFF] = 0x02;
    rom[0x8000] = 0x03;
    rom[0x8001] = 0x04;
    SnesBackend backend;
    backend.loadRom(rom);

    const MemoryRegion pairs{.at = 0x00FFFEu, .size = 2, .count = 2};
    ASSERT_TRUE(backend.regionIsAddressable(pairs));
    std::array<std::uint8_t, 2> entry{};
    backend.readRegion(pairs, 1, entry);
    EXPECT_EQ(entry, (std::array<std::uint8_t, 2>{0x03, 0x04}));

    const std::array<std::uint8_t, 2> patch{0x13, 0x14};
    backend.writeRegion(pairs, 1, patch);
    EXPECT_EQ(backend.readMemory(0x018000u, 2), 0x1413u);
}

TEST(SnesBackendPlaces, BusAddressOfAnswersAnAddressThatReachesTheByte) {
    struct Case {
        const char*                name;
        snaggletooth::CartridgeMap map;
    };
    for (const Case c : {Case{.name = "LoROM", .map = snaggletooth::CartridgeMap::LoRom},
                         Case{.name = "HiROM", .map = snaggletooth::CartridgeMap::HiRom},
                         Case{.name = "ExHiROM", .map = snaggletooth::CartridgeMap::ExHiRom}}) {
        const std::vector<std::uint8_t> image(0x10000u, 0u);
        const snaggletooth::Snes machine{
            snaggletooth::SnesConfig{.rom = image, .map = c.map, .saveRamBytes = 0x2000u}};
        using Space = snaggletooth::Snes::Space;
        const auto roundTrips = [&](Space space, std::uint32_t count) {
            for (std::uint32_t i = 0; i < count; ++i) {
                const snaggletooth::Snes::Physical place{.space = space, .index = i};
                const std::optional<std::uint32_t> address = vm::snes_address::busAddressOf(c.map, place);
                ASSERT_TRUE(address.has_value()) << c.name << " offset " << i;
                ASSERT_EQ(machine.physical(*address), place) << c.name << " offset " << i;
            }
        };
        roundTrips(Space::CartridgeRom, 0x10000u);
        roundTrips(Space::SaveRam, 0x2000u);
        roundTrips(Space::WorkRam, 0x20000u);
        EXPECT_EQ(vm::snes_address::busAddressOf(c.map, {.space = Space::Register, .index = 0x2100u}), 0x002100u);
        EXPECT_FALSE(vm::snes_address::busAddressOf(c.map, {.space = Space::OpenBus, .index = 0x4000u}));
    }
}

TEST(SnesBackendPlaces, ARegisterReadsAsItStandsAndTakesNoWrite) {
    SnesBackend backend;
    backend.loadRom(loRomWithSave());
    for (const std::uint32_t reg : {0x002100u, 0x002140u, 0x004210u, 0x804218u}) {
        EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = reg, .size = 1})) << reg;
    }
    // The values a read answers, read twice with nothing moved: RDNMI's low nibble is the CPU's version
    // and STAT77's the picture chip's, and a register answers at its offset in every system bank.
    EXPECT_EQ(backend.readMemory(0x004210, 1) & 0x0F, 2u);
    EXPECT_EQ(backend.readMemory(0x004210, 1), backend.readMemory(0x804210, 1));
    EXPECT_EQ(backend.readMemory(0x00213E, 1) & 0x0F, 1u);
    // A run over the register windows is every byte a register; one that reaches the open bus between
    // them is not a place.
    EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = 0x004200, .size = 0x20}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = 0x004210, .size = 0x1000}));
    EXPECT_THROW(backend.writeMemory(0x004200, 0x81, 1), std::logic_error);
}

TEST(SnesBackendPlaces, OpenBusIsNotAPlace) {
    SnesBackend backend;
    backend.loadRom(loRomWithSave());
    // Between the register windows, and a system bank's expansion page: no memory answers either.
    for (const std::uint32_t open : {0x004000u, 0x006000u}) {
        EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = open, .size = 1})) << open;
        EXPECT_THROW(backend.writeMemory(open, 0, 1), std::out_of_range) << open;
    }
    // A machine hosting nothing has no memory at all.
    SnesBackend empty;
    EXPECT_FALSE(empty.regionIsAddressable(snes::WorkRam));
}

TEST(SnesBackendPlaces, TheMemoriesTheBusCannotNameReadWholeAndTakeAByteByName) {
    SnesBackend backend;
    backend.loadRom(loRomWithSave());

    struct Case {
        MemoryRegion  whole;
        std::uint32_t place;
        std::size_t   offset;
    };
    for (const Case c : {Case{.whole = snes::VideoRam, .place = snes::videoRam(0x1234), .offset = 0x1234},
                         Case{.whole = snes::Palette, .place = snes::palette(0x101), .offset = 0x101},
                         Case{.whole = snes::Sprites, .place = snes::sprites(0x21F), .offset = 0x21F},
                         Case{.whole = snes::AudioRam, .place = snes::audioRam(0x4321), .offset = 0x4321}}) {
        ASSERT_TRUE(backend.regionIsAddressable(c.whole)) << c.whole.at;
        std::vector<std::uint8_t> all(c.whole.size);
        backend.readRegion(c.whole, 0, all);

        backend.writeMemory(c.place, 0x5C, 1);
        EXPECT_EQ(byteAt(backend, c.place), 0x5Cu) << c.whole.at;
        backend.readRegion(c.whole, 0, all);
        EXPECT_EQ(all[c.offset], 0x5Cu) << c.whole.at;
    }

    // Each landed in the memory it names, written by name.
    const snaggletooth::SnesState& state = SnesBackendTestAccess::state(backend);
    EXPECT_EQ(state.ppu.vram[0x1234], 0x5Cu);
    EXPECT_EQ(state.ppu.cgram[0x101], 0x5Cu);
    EXPECT_EQ(state.ppu.oam[0x21F], 0x5Cu);
    EXPECT_EQ(state.apu.ram[0x4321], 0x5Cu);
}

TEST(SnesBackendPlaces, OneBytePastEachMemoryIsRefused) {
    SnesBackend backend;
    backend.loadRom(loRomWithSave());

    for (const MemoryRegion& m : {snes::WorkRam, snes::VideoRam, snes::Palette, snes::Sprites, snes::AudioRam}) {
        EXPECT_TRUE(backend.regionIsAddressable(m)) << m.at;
        EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = m.at, .size = m.size + 1})) << m.at;
    }
    EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = 0x700000u, .size = 0x2000u}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = 0x700000u, .size = 0x2001u}));
    EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = 0x008000u, .size = 0x8000u}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = 0x008000u, .size = 0x8001u}));

    // Past a memory's own end, and a top byte that names no memory on this console.
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::palette(0x200), .size = 1}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::sprites(0x220), .size = 1}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = 0x05000000u, .size = 1}));
}

}  // namespace
}  // namespace retropp
