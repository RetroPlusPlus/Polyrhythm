// The audio unit as places: the console's side of the four comm ports (snes::audioPort) and the sound
// chip's 128 DSP registers (snes::dspRegister), reached through the region verbs the way the picture chip's
// memories are.
//
// What these places have to get right, one case each: a port write reaches the sound CPU's read latch and
// not the bus; a port read receives the byte the sound CPU last sent; the two latches are distinct, so a
// port is not a byte written and read back; a run spans the four ports; a DSP register reads as it stands
// and a write to it is the sound CPU's own write to the chip; and each space ends where it ends.

#include <array>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "examples/common.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

// A one-bank LoROM image, enough to give the machine its audio unit; a place is read and written without
// running anything.
SnesBackend hostedBackend() {
    SnesBackend backend;
    backend.loadRom(snaggletooth::examples::loRomImage(1));
    return backend;
}

TEST(SnesApuPlaces, APortWriteReachesTheSoundCpusReadLatchAndNotTheBus) {
    SnesBackend backend = hostedBackend();

    backend.writeMemory(snes::audioPort(1), 0x5A, 1);
    EXPECT_EQ(SnesBackendTestAccess::apuInputPort(backend, 1), 0x5Au);  // the sound CPU reads it at $F5
    // The write went to the port, not to the bus: work RAM byte $01 (a plain bus address) is untouched.
    EXPECT_EQ(SnesBackendTestAccess::state(backend).wram[1], 0x00u);
}

TEST(SnesApuPlaces, APortReadReceivesWhatTheSoundCpuSent) {
    SnesBackend backend = hostedBackend();

    SnesBackendTestAccess::setApuOutputPort(backend, 2, 0xA5);  // the byte the sound CPU sent on port 2
    std::array<std::uint8_t, 1> got{};
    backend.readRegion(MemoryRegion{.at = snes::audioPort(2), .size = 1}, 0, got);
    EXPECT_EQ(got[0], 0xA5u);
}

TEST(SnesApuPlaces, APortsTwoLatchesAreDistinct) {
    // A comm port is asymmetric: writing sends to the sound CPU (the input latch), reading receives from it
    // (the output latch). A value written is not the value read back.
    SnesBackend backend = hostedBackend();

    SnesBackendTestAccess::setApuOutputPort(backend, 0, 0x11);  // what the sound CPU sent
    backend.writeMemory(snes::audioPort(0), 0x22, 1);           // what the console sends

    EXPECT_EQ(backend.readMemory(snes::audioPort(0), 1), 0x11u);       // the read sees the sound CPU's byte
    EXPECT_EQ(SnesBackendTestAccess::apuInputPort(backend, 0), 0x22u);  // the write reached the other latch
}

TEST(SnesApuPlaces, ARunSpansTheFourPorts) {
    SnesBackend backend = hostedBackend();

    for (std::uint8_t i = 0; i < 4; ++i) {
        SnesBackendTestAccess::setApuOutputPort(backend, i, static_cast<std::uint8_t>(0xF0 + i));
    }
    const MemoryRegion ports{.at = snes::audioPort(0), .size = 4};
    ASSERT_TRUE(backend.regionIsAddressable(ports));
    std::array<std::uint8_t, 4> got{};
    backend.readRegion(ports, 0, got);
    EXPECT_EQ(got, (std::array<std::uint8_t, 4>{0xF0, 0xF1, 0xF2, 0xF3}));

    // There are four ports and no fifth: a run one byte longer is not a place.
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::audioPort(0), .size = 5}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::audioPort(4), .size = 1}));
}

TEST(SnesApuPlaces, ADspRegisterReadsAsItStands) {
    SnesBackend backend = hostedBackend();

    SnesBackendTestAccess::setDspRegister(backend, 0x4C, 0x33);  // KON, as the sound program left it
    EXPECT_EQ(backend.readMemory(snes::dspRegister(0x4C), 1), 0x33u);

    // A run spans the whole register file, and there are 128 of them and no more.
    EXPECT_TRUE(backend.regionIsAddressable(MemoryRegion{.at = snes::dspRegister(0), .size = 128}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::dspRegister(0), .size = 129}));
    EXPECT_FALSE(backend.regionIsAddressable(MemoryRegion{.at = snes::dspRegister(128), .size = 1}));
}

TEST(SnesApuPlaces, ADspRegisterWriteIsTheSoundCpusOwnWrite) {
    // A write through the place is the sound CPU's write to the chip: the byte reads back, and a key-on
    // arms the voices the chip's next poll takes — the machine's own effect, not a stored byte.
    SnesBackend backend = hostedBackend();

    backend.writeMemory(snes::dspRegister(0x0C), 0x60, 1);  // MVOLL
    EXPECT_EQ(backend.readMemory(snes::dspRegister(0x0C), 1), 0x60u);
    EXPECT_EQ(SnesBackendTestAccess::state(backend).apu.dsp.regs[0x0C], 0x60u);

    backend.writeRegion(MemoryRegion{.at = snes::dspRegister(0x4C), .size = 1}, 0,
                        std::array<std::uint8_t, 1>{0x03});  // KON: voices 0 and 1
    EXPECT_EQ(SnesBackendTestAccess::state(backend).apu.dsp.internalKon, 0x03u);
}

// A single byte is the one-byte case of a region, so the question a routine's memory binding asks
// (addressIsAccessible) and the question a declared place asks are the same — the new spaces included.
TEST(SnesApuPlaces, APlaceAndABindingAgreeAboutTheAudioUnit) {
    SnesBackend backend = hostedBackend();
    struct Case {
        std::uint32_t address;
        bool          accessible;
    };
    for (const Case c : {Case{.address = snes::audioPort(0), .accessible = true},
                         Case{.address = snes::audioPort(3), .accessible = true},
                         Case{.address = snes::audioPort(4), .accessible = false},
                         Case{.address = snes::dspRegister(0), .accessible = true},
                         Case{.address = snes::dspRegister(127), .accessible = true},
                         Case{.address = snes::dspRegister(128), .accessible = false}}) {
        EXPECT_EQ(backend.addressIsAccessible(c.address), c.accessible) << "address " << c.address;
    }
}

}  // namespace
}  // namespace retropp
