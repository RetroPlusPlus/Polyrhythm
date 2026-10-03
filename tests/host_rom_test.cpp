// Hosting a whole cartridge image the game supplies: its bytes become addressable, and the two ways
// of owning a cartridge refuse each other.
//
// The cartridges here are authored by these tests, byte for byte. SameBoy's buffer loader validates
// nothing — no logo, no checksum — and reads only the cartridge-type and ROM-size header bytes to
// pick a mapper, so an image is the planted pattern plus those two fields. An authored image is also
// the only way to assert against known content: with a real cartridge there is no ground truth, and
// the assertions would ratify whatever bytes happened to be there.
#include "src/vm/gameboy/sameboy_backend.h"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"
#include "tests/authored_cartridge.h"

namespace retropp::vm {
namespace {

using retropp::testing::authorCartridge;
using retropp::testing::kSmallestCartridge;

// A resident-driver image the engine places into a cartridge it builds itself.
constexpr std::array<std::uint8_t, 1> kDriverBytes{0xC9};  // ret

TEST(HostRom, MakesTheImagesBytesAddressable) {
    std::vector<std::uint8_t> rom = authorCartridge(kSmallestCartridge);
    rom[0x2000] = 0xBE;
    rom[0x2001] = 0xEF;

    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    backend.loadRom(rom);

    EXPECT_EQ(backend.readMemory(0x2000, 1), 0xBEu);
    EXPECT_EQ(backend.readMemory(0x2001, 1), 0xEFu);
}

TEST(HostRom, ReadsTheImageThatWasLoadedMostRecently) {
    std::vector<std::uint8_t> first = authorCartridge(kSmallestCartridge);
    first[0x2000] = 0x11;

    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    backend.loadRom(first);
    EXPECT_EQ(backend.readMemory(0x2000, 1), 0x11u);

    std::vector<std::uint8_t> second = authorCartridge(kSmallestCartridge);
    second[0x2000] = 0x22;
    backend.loadRom(second);
    EXPECT_EQ(backend.readMemory(0x2000, 1), 0x22u);
}

TEST(HostRom, AnEmptyImageIsRefused) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    EXPECT_THROW(backend.loadRom(std::span<const std::uint8_t>{}), std::invalid_argument);
}

TEST(HostRom, RefusesACartridgeTheEngineAlreadyBuilt) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    const std::array<DriverImage, 1> images{
        DriverImage{.bytes = std::span<const std::uint8_t>(kDriverBytes), .base = 0x4000}};
    backend.configureResidentImage(images, gb::Mbc3, /*stackTop=*/0);

    const std::vector<std::uint8_t> rom = authorCartridge(kSmallestCartridge);
    EXPECT_THROW(backend.loadRom(rom), std::logic_error);
}

TEST(HostRom, ADriverCannotBeHostedOnTheGamesCartridge) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    backend.loadRom(authorCartridge(kSmallestCartridge));

    const std::array<DriverImage, 1> images{
        DriverImage{.bytes = std::span<const std::uint8_t>(kDriverBytes), .base = 0x4000}};
    EXPECT_THROW(backend.configureResidentImage(images, gb::Mbc3, /*stackTop=*/0), std::logic_error);
}

TEST(HostRom, RoutinesCannotBePlacedIntoTheGamesCartridge) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    backend.loadRom(authorCartridge(kSmallestCartridge));

    EXPECT_THROW(backend.placeRoutine(std::span<const std::uint8_t>(kDriverBytes), std::nullopt),
                 std::logic_error);
}

// The same refusal at the surface a game actually calls: a hosted cartridge has no arena, so
// registering a routine on that VM is refused rather than silently corrupting the image.
TEST(HostRom, RegisteringARoutineOnAHostedCartridgeIsRefused) {
    retropp::Vm::GBC vm;
    vm.hostRom(authorCartridge(kSmallestCartridge));

    EXPECT_THROW((void)vm.uploadRoutine<void()>(std::span<const std::uint8_t>(kDriverBytes),
                                                retropp::RoutineBinding{}),
                 std::logic_error);
}

TEST(HostRom, ARoutinePlacedBeforeHostingIsNotCarriedIntoTheGamesCartridge) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    const std::uint32_t entry =
        backend.placeRoutine(std::span<const std::uint8_t>(kDriverBytes), std::nullopt);

    std::vector<std::uint8_t> rom = authorCartridge(kSmallestCartridge);
    rom[entry] = 0x00;  // the game's own byte at that address
    backend.loadRom(rom);

    // The game owns every byte of its cartridge; nothing the engine placed earlier survives into it.
    EXPECT_EQ(backend.readMemory(entry, 1), 0x00u);
}

}  // namespace
}  // namespace retropp::vm
