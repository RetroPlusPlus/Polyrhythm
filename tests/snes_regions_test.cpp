// The SNES's own memories, shipped as MemoryRegion constants.
//
// A console header supplies these exactly as gb.h supplies the Game Boy's — the same value a game fills
// in for its own content, filled in here for the hardware. So the cases below check the two things a
// constant has to get right: that it resolves to the memory it names, and that its extent is that
// memory's whole extent and not a byte more.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "examples/snes/cartridge/cartridge.h"
#include "retropp/guest_watch.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "retropp/vm.h"
#include "src/vm/snes/snes_backend.h"

namespace retropp {
namespace {

struct Places {
    MemoryRegion palette;
    MemoryRegion work;
};

Vm::SNES hostedVm() {
    Vm::SNES vm;
    vm.hostRom(examples::snes::demoCartridge());
    return vm;
}

TEST(SnesRegions, EachMemoryReadsItsWholeExtentInOneGo) {
    Vm::SNES vm = hostedVm();

    EXPECT_EQ(vm.read(snes::WorkRam).size(), 0x20000u);
    EXPECT_EQ(vm.read(snes::VideoRam).size(), 0x10000u);
    EXPECT_EQ(vm.read(snes::Palette).size(), 0x200u);
    EXPECT_EQ(vm.read(snes::Sprites).size(), 0x220u);
    EXPECT_EQ(vm.read(snes::AudioRam).size(), 0x10000u);
}

TEST(SnesRegions, AWholeMemoryIsOneEntry) {
    // count defaults to 1, so a whole memory is the degenerate array and read() needs no index.
    EXPECT_EQ(snes::VideoRam.count, 1u);
    EXPECT_EQ(snes::VideoRam.totalBytes(), 0x10000u);
    EXPECT_TRUE(snes::VideoRam.contains(0));
    EXPECT_FALSE(snes::VideoRam.contains(1));
}

TEST(SnesRegions, WorkRamRoundTripsThroughTheConstant) {
    Vm::SNES vm = hostedVm();

    std::vector<std::uint8_t> whole = vm.read(snes::WorkRam);
    whole[0x10123] = 0x5A;  // in bank $7F, past the half the low pages mirror
    vm.write(snes::WorkRam, whole);

    EXPECT_EQ(vm.read(snes::WorkRam)[0x10123], 0x5Au);
}

TEST(SnesRegions, VideoRamRoundTripsThroughSnesVideoRam) {
    Vm::SNES vm = hostedVm();

    std::vector<std::uint8_t> whole = vm.read(snes::VideoRam);
    whole[0x2010] = 0x3C;
    vm.write(snes::VideoRam, whole);

    EXPECT_EQ(vm.read(MemoryRegion{.at = snes::videoRam(0x2010), .size = 1}).at(0), 0x3Cu);

    vm.write(MemoryRegion{.at = snes::videoRam(0x2011), .size = 1}, std::vector<std::uint8_t>{0xC3});
    EXPECT_EQ(vm.read(snes::VideoRam)[0x2011], 0xC3u);
}

// A constant and a place named by address inside it must agree about where they are — through any alias.
TEST(SnesRegions, APlaceInsideAMemoryLandsWhereTheConstantSaysItDoes) {
    Vm::SNES vm = hostedVm();

    vm.write(MemoryRegion{.at = 0x7E0123, .size = 1}, std::vector<std::uint8_t>{0xA7});
    EXPECT_EQ(vm.read(snes::WorkRam)[0x0123], 0xA7u);

    vm.write(MemoryRegion{.at = 0x000124, .size = 1}, std::vector<std::uint8_t>{0xA8});
    EXPECT_EQ(vm.read(snes::WorkRam)[0x0124], 0xA8u);
}

TEST(SnesRegions, AConstantCanBeDeclaredInABatchLikeAnyOtherPlace) {
    Vm::SNES vm = hostedVm();

    const auto places = vm.registerRegions(regions(
        region(&Places::palette, snes::Palette, "palette"),
        region(&Places::work, snes::WorkRam, "work ram")));

    EXPECT_EQ(places.size(), 2u);
    EXPECT_EQ(places.declared(&Places::palette)->at, snes::palette(0));
    EXPECT_EQ(vm.read(places, &Places::work).size(), 0x20000u);
}

TEST(SnesRegions, AMemoryDoesNotDeclareASecondEntry) {
    Vm::SNES vm = hostedVm();

    EXPECT_THROW((void)vm.read(snes::Sprites, 1), std::out_of_range);
}

// Each constant's extent stops at the end of its memory: one byte more does not resolve.
TEST(SnesRegions, OneByteBeyondAMemoryIsRefused) {
    Vm::SNES vm = hostedVm();

    for (const MemoryRegion& m : {snes::WorkRam, snes::VideoRam, snes::Palette, snes::Sprites, snes::AudioRam}) {
        const MemoryRegion tooLong{.at = m.at, .size = m.size + 1};
        EXPECT_THROW((void)vm.registerRegions(regions(region(&Places::palette, tooLong, "too long"))),
                     std::invalid_argument);
    }
}

// ── One answer about what memory exists ─────────────────────────────────────────────────────────
// A single byte is the one-byte case of a region, so the question a routine's memory binding asks
// (addressIsAccessible) and the question a declared place asks are the SAME question. This pins that:
// whatever a place may name on this console, the backend answers a binding the same, address for address.

bool aPlaceMayNameIt(Vm& vm, std::uint32_t address) {
    try {
        (void)vm.registerRegions(
            regions(region(&Places::palette, MemoryRegion{.at = address, .size = 1}, "one byte")));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

TEST(SnesRegions, APlaceAndABindingAgreeAboutEveryAddress) {
    vm::SnesBackend backend;
    backend.loadRom(examples::snes::demoCartridge());
    for (const std::uint32_t address : {std::uint32_t{0x008000},          // the image
                                        std::uint32_t{0x808000},          // the image, in an upper bank
                                        std::uint32_t{0x001FFF},          // work ram, a low-page alias
                                        std::uint32_t{0x7FFFFF},          // work ram, the last byte
                                        std::uint32_t{0x700000},          // the save
                                        std::uint32_t{0x002100},          // a register: served by neither
                                        std::uint32_t{0x004000},          // open bus: served by neither
                                        snes::videoRam(0xFFFF),           // video ram, the last byte
                                        snes::palette(0x1FF),             // the palette, the last byte
                                        snes::palette(0x200),             // one past it: served by neither
                                        snes::sprites(0x21F),             // the sprite table, the last byte
                                        snes::audioRam(0x0000),           // the audio unit's RAM
                                        std::uint32_t{0x05000000}}) {     // no memory: served by neither
        Vm::SNES placeVm = hostedVm();
        EXPECT_EQ(aPlaceMayNameIt(placeVm, address), backend.addressIsAccessible(address))
            << "the two answers disagree about address " << address;
    }
}

// A watch on the game's own reads and writes is answered in the flat space the CPU sees, which a memory
// the bus cannot name is not in.
TEST(SnesRegions, APlaceInAMemoryTheBusCannotNameMayNotBeWatchedForTheGamesOwnAccesses) {
    Vm::SNES vm = hostedVm();
    try {
        vm.registerWatches(retropp::watches(
            GuestWatch{.key     = "video",
                       .at      = MemoryRegion{.at = snes::videoRam(0), .size = 1},
                       .from    = AccessSource::GuestAndGame,
                       .onWrite = [](Vm&, std::uint32_t, std::uint8_t) { return AccessVerdict::proceed(); }}));
        FAIL() << "a GuestAndGame watch on video ram should not register";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("bank-qualified"), std::string::npos) << e.what();
        EXPECT_EQ(std::string(e.what()).find("not reachable"), std::string::npos) << e.what();
    }
}

}  // namespace
}  // namespace retropp
