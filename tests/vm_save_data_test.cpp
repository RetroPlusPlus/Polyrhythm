// What a keyed machine keeps for the player, end to end: a guest writes into its cartridge's
// battery-backed memory, the bytes reach a file under the player's own files, and a machine hosting
// that cartridge again comes up on them.
//
// Every case runs the REAL core over an authored cartridge, because the thing under test is a chain
// that only exists when all of it is real: the guest's store raises the core's own change signal, the
// host layer notices it at a step boundary, and what lands on disk is what the cartridge held. A
// scripted double would prove the host layer's arithmetic and none of that.
//
// The files are rooted at a temp directory through the same seam UserFiles::atPath is, so no case
// touches the machine's real data directory or leaves anything behind.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "retropp/memory_region.h"
#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

// The byte of high RAM a reader routine parks its answer in, and the place that reads it back out.
const MemoryRegion kAnswer{.at = 0xFF80, .size = 1, .count = 1};

std::string hex(std::uint8_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return std::string{kDigits[value >> 4], kDigits[value & 0x0F]};
}

// A cartridge that runs `source` from $0150. The entry jumps there, the VBlank vector returns, and a
// lone ret sits past the CGB overlay window so code that wanders returns instead of running away.
std::vector<std::uint8_t> cartridgeRunning(Vm& assembler, const std::string& source,
                                           std::uint8_t cartridgeType, std::uint8_t ramSizeByte) {
    std::vector<std::uint8_t> rom =
        testing::authorCartridge(testing::kSmallestCartridge, cartridgeType, ramSizeByte);
    rom[0x040] = 0xD9;  // VBlank: reti
    rom[0x100] = 0x00;  // entry: nop
    rom[0x101] = 0xC3;
    rom[0x102] = 0x50;
    rom[0x103] = 0x01;  // jp $0150
    rom[0x900] = 0xC9;  // ret past the CGB overlay window
    const std::vector<std::uint8_t> code = assembler.assemble(source);
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

// Unlock the cartridge's own RAM (MBC3 takes $0A at $0000) and store `value` in its first byte. This
// store is the whole feature's reason to exist.
std::string writesToItsCartridge(std::uint8_t value) {
    return "    ld a, $0A\n"
           "    ld [$0000], a\n"
           "    ld a, $" + hex(value) + "\n"
           "    ld [$A000], a\n"
           "loop:\n"
           "    jr loop\n";
}

// The same store, performed on EVERY pass of the loop rather than once. The bytes never change after
// the first pass, but the core reports its save memory touched for as long as this runs — which is the
// guest a real cartridge's checksum-rewriting code is, and the only guest that can tell whether the
// host writes on a raised signal or on a genuine change.
std::string writesTheSameByteForever(std::uint8_t value) {
    return "    ld a, $0A\n"
           "    ld [$0000], a\n"
           "    ld a, $" + hex(value) + "\n"
           "loop:\n"
           "    ld [$A000], a\n"
           "    jr loop\n";
}

// Unlock the cartridge's own RAM and copy its first byte into high RAM, where a declared place can
// read it. This is how a case sees what a machine CAME UP holding, rather than what it was handed.
constexpr const char* kReadsItsCartridge =
    "    ld a, $0A\n"
    "    ld [$0000], a\n"
    "    ld a, [$A000]\n"
    "    ldh [$80], a\n"
    "loop:\n"
    "    jr loop\n";

class VmSaveDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = std::filesystem::temp_directory_path() /
                (std::string("retropp-vm-save-test-") + info->name());
        std::filesystem::remove_all(root_);
    }
    void TearDown() override { std::filesystem::remove_all(root_); }

    // Step the machine on the calling thread. Inline stepping is what makes the chain deterministic —
    // no thread of its own, and no interval to wait out.
    static void step(Vm& machine, int steps) {
        for (int i = 0; i < steps; ++i) {
            vm::VmTestAccess::stepOnce(machine);
        }
    }

    // Run a hosted cartridge for a few steps and put the machine away, which is what writes.
    static void runAndPutAway(Vm& machine, int steps = 4) {
        vm::VmTestAccess::runInline(machine);
        step(machine, steps);
        machine.stop();
    }

    [[nodiscard]] std::filesystem::path fileFor(const std::string& key,
                                                const std::string& name) const {
        return root_ / "VM" / key / name;
    }

    [[nodiscard]] static std::vector<std::uint8_t> contentsOf(const std::filesystem::path& at) {
        std::ifstream in(at, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                         std::istreambuf_iterator<char>());
    }

    std::filesystem::path root_;
};

// ── The chain, end to end ───────────────────────────────────────────────────────────────────────

TEST_F(VmSaveDataTest, WhatAGuestWritesIsWhatTheNextMachineComesUpOn) {
    Vm assembler{VMPlatform::GameBoyColor};
    const std::vector<std::uint8_t> writing = cartridgeRunning(
        assembler, writesToItsCartridge(0x5A), testing::kMbc3RamBattery, testing::kRam8K);
    const std::vector<std::uint8_t> reading = cartridgeRunning(
        assembler, kReadsItsCartridge, testing::kMbc3RamBattery, testing::kRam8K);

    {
        Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
        vm::VmTestAccess::saveFilesAt(machine, root_);
        machine.hostRom(writing);
        runAndPutAway(machine);
    }

    // The file is the player's, sitting where the game's other files for them do.
    const std::filesystem::path at = fileFor("cartridge", "battery.sav");
    ASSERT_TRUE(std::filesystem::exists(at)) << "nothing was kept at " << at;
    const std::vector<std::uint8_t> kept = contentsOf(at);
    ASSERT_FALSE(kept.empty());
    EXPECT_EQ(kept[0], 0x5A) << "the byte the guest stored is not the byte on disk";

    // ...and a machine hosting that cartridge again finds it there. The second guest reads its OWN
    // cartridge RAM, so what it answers with is what the cartridge came up holding — not anything
    // this case put in front of it. It also pins that the stored copy survives the boot run() performs:
    // the guest does not execute at all until after that reset, so a reset that wiped the cartridge's
    // RAM would leave this reading whatever a blank cartridge reads.
    Vm next{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    vm::VmTestAccess::saveFilesAt(next, root_);
    next.hostRom(reading);
    runAndPutAway(next);
    EXPECT_EQ(next.read(kAnswer, 0).at(0), 0x5A)
        << "the machine did not come up on what the last one kept";
}

// ── What a machine with no key does, which is nothing ───────────────────────────────────────────

TEST_F(VmSaveDataTest, AMachineWithNoKeyKeepsNothingAtAll) {
    Vm assembler{VMPlatform::GameBoyColor};
    const std::vector<std::uint8_t> writing = cartridgeRunning(
        assembler, writesToItsCartridge(0x5A), testing::kMbc3RamBattery, testing::kRam8K);

    Vm machine{VMPlatform::GameBoyColor};  // no key — the default every shipped machine has
    vm::VmTestAccess::saveFilesAt(machine, root_);
    machine.hostRom(writing);
    runAndPutAway(machine);

    // Not an empty file, not an empty directory — nothing was created.
    EXPECT_FALSE(std::filesystem::exists(root_ / "VM"))
        << "a machine with no key wrote into the player's files";
}

TEST_F(VmSaveDataTest, ACartridgeWithNoBatteryKeepsNothingEvenWhenNamed) {
    Vm assembler{VMPlatform::GameBoyColor};
    // The same guest code on a cartridge with nowhere to keep anything: its stores reach a mapper with
    // no RAM behind it, which is what the hardware does with them.
    const std::vector<std::uint8_t> writing =
        cartridgeRunning(assembler, writesToItsCartridge(0x5A), testing::kRomOnly, testing::kNoRam);

    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    vm::VmTestAccess::saveFilesAt(machine, root_);
    machine.hostRom(writing);
    runAndPutAway(machine);

    EXPECT_FALSE(std::filesystem::exists(fileFor("cartridge", "battery.sav")))
        << "a cartridge that keeps nothing still produced a file";
}

// ── A file is rewritten only when rewriting it would change it ───────────────────────────────────

TEST_F(VmSaveDataTest, DataThatDidNotChangeIsNotWrittenAgain) {
    Vm assembler{VMPlatform::GameBoyColor};
    // A guest that stores the SAME byte on every pass: the core reports its save memory touched for as
    // long as this runs, so a host that wrote on the raised signal would rewrite the file forever.
    const std::vector<std::uint8_t> writing = cartridgeRunning(
        assembler, writesTheSameByteForever(0x5A), testing::kMbc3RamBattery, testing::kRam8K);

    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    vm::VmTestAccess::saveFilesAt(machine, root_);
    machine.hostRom(writing);
    vm::VmTestAccess::runInline(machine);
    step(machine, 4);
    vm::VmTestAccess::flushSaveData(machine);

    const std::filesystem::path at = fileFor("cartridge", "battery.sav");
    ASSERT_TRUE(std::filesystem::exists(at));

    // Taking the file away is how a second write becomes visible. The guest keeps running, and the
    // core keeps reporting that its RAM was touched — a store of the same byte is still a store — but
    // the bytes are the ones already on disk. A file that comes back is a machine rewriting a save it
    // is not changing, which on a large cartridge is a rewrite of the whole thing for nothing.
    std::filesystem::remove(at);
    step(machine, 8);
    vm::VmTestAccess::flushSaveData(machine);
    machine.stop();

    EXPECT_FALSE(std::filesystem::exists(at))
        << "the machine rewrote a file whose contents it was not changing";
}

// ── One machine, many cartridges ─────────────────────────────────────────────────────────────────

TEST_F(VmSaveDataTest, EachCartridgeGetsItsOwnFileAndKeepsIt) {
    Vm assembler{VMPlatform::GameBoyColor};
    const std::vector<std::uint8_t> first = cartridgeRunning(
        assembler, writesToItsCartridge(0x11), testing::kMbc3RamBattery, testing::kRam8K);
    const std::vector<std::uint8_t> second = cartridgeRunning(
        assembler, writesToItsCartridge(0x22), testing::kMbc3RamBattery, testing::kRam8K);

    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "player"}};
    vm::VmTestAccess::saveFilesAt(machine, root_);

    machine.batterySave("first-image");
    machine.hostRom(first);
    runAndPutAway(machine);

    // The same machine, handed a different cartridge — what one image keeps must not land in another
    // image's file.
    machine.batterySave("second-image");
    machine.hostRom(second);
    runAndPutAway(machine);

    const std::vector<std::uint8_t> keptFirst  = contentsOf(fileFor("player", "first-image.sav"));
    const std::vector<std::uint8_t> keptSecond = contentsOf(fileFor("player", "second-image.sav"));
    ASSERT_FALSE(keptFirst.empty());
    ASSERT_FALSE(keptSecond.empty());
    EXPECT_EQ(keptFirst[0], 0x11) << "the first cartridge's file was overwritten by the second's";
    EXPECT_EQ(keptSecond[0], 0x22);
}

// ── The file always ends with the core's own extension ──────────────────────────────────────────

TEST_F(VmSaveDataTest, TheCoresExtensionAlwaysEndsTheFileAndIsNeverDoubled) {
    Vm assembler{VMPlatform::GameBoyColor};
    const std::vector<std::uint8_t> writing = cartridgeRunning(
        assembler, writesToItsCartridge(0x5A), testing::kMbc3RamBattery, testing::kRam8K);

    // Three names a developer might give for the same intent. What lands on disk carries the Game Boy
    // family's own extension in every case — the file is one any other program reading a Game Boy save
    // already expects, whether or not the developer thought about it.
    const std::pair<const char*, const char*> cases[] = {
        {"spelled-out.sav", "spelled-out.sav"},  // already right: left exactly as given
        {"left-off", "left-off.sav"},            // added
        {"other.bak", "other.bak.sav"},          // kept, and the core's added after it
    };

    for (const auto& [given, onDisk] : cases) {
        Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
        vm::VmTestAccess::saveFilesAt(machine, root_);
        machine.batterySave(given);
        machine.hostRom(writing);
        runAndPutAway(machine);

        EXPECT_TRUE(std::filesystem::exists(fileFor("cartridge", onDisk)))
            << "given \"" << given << "\", nothing landed at \"" << onDisk << "\"";
    }
}

TEST_F(VmSaveDataTest, TheDefaultFileIsTheOnlyOneAMachineNeeds) {
    Vm assembler{VMPlatform::GameBoyColor};
    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    vm::VmTestAccess::saveFilesAt(machine, root_);
    machine.hostRom(cartridgeRunning(assembler, writesToItsCartridge(0x33),
                                     testing::kMbc3RamBattery, testing::kRam8K));
    runAndPutAway(machine);

    // Nothing named it, so it is "battery" — what a machine playing one cartridge never has to think
    // about.
    EXPECT_TRUE(std::filesystem::exists(fileFor("cartridge", "battery.sav")));
}

// ── Keys and names that are not one path component ──────────────────────────────────────────────

TEST_F(VmSaveDataTest, AMachinesKeyIsOnePathComponent) {
    // An omitted key is not a bad key — it is a machine that keeps nothing, which is the default.
    EXPECT_NO_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{}));
    EXPECT_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{.key = "."}), std::invalid_argument);
    EXPECT_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{.key = ".."}), std::invalid_argument);
    EXPECT_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{.key = "over/there"}), std::invalid_argument);
    EXPECT_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{.key = "..\\elsewhere"}),
                 std::invalid_argument);
    EXPECT_NO_THROW(Vm(VMPlatform::GameBoyColor, VmConfig{.key = "a perfectly ordinary name"}));
}

TEST_F(VmSaveDataTest, ABatterySavesNameIsOnePathComponentToo) {
    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    EXPECT_THROW(machine.batterySave(""), std::invalid_argument);
    EXPECT_THROW(machine.batterySave(".."), std::invalid_argument);
    EXPECT_THROW(machine.batterySave("down/one"), std::invalid_argument);
    EXPECT_NO_THROW(machine.batterySave("oracle-of-ages"));
}

TEST_F(VmSaveDataTest, AMachineWithNoKeyHasNoDirectoryToPutAFileIn) {
    Vm machine{VMPlatform::GameBoyColor};
    EXPECT_THROW(machine.batterySave("anything"), std::logic_error);
}

TEST_F(VmSaveDataTest, ANameCannotMoveWhileTheGuestIsWritingIntoIt) {
    Vm assembler{VMPlatform::GameBoyColor};
    Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
    vm::VmTestAccess::saveFilesAt(machine, root_);
    machine.hostRom(cartridgeRunning(assembler, writesToItsCartridge(0x44),
                                     testing::kMbc3RamBattery, testing::kRam8K));
    vm::VmTestAccess::runInline(machine);

    EXPECT_THROW(machine.batterySave("somewhere-else"), std::logic_error);
    machine.stop();
    EXPECT_NO_THROW(machine.batterySave("somewhere-else"));  // parked, it is an ordinary thing to ask
}

}  // namespace
}  // namespace retropp
