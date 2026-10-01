// The SNES audio unit alone: the machine an AudioSystem::SNES of the HostDriven kind hosts — the sound
// CPU, its RAM and the sound chip, with no 5A22 and no picture chip built around them.
//
// The cases: the unit comes up as the seeded post-boot machine; the three audio places land on it by the
// same names and the comm ports keep their two latches; a DSP write is the chip's own; its clock is its
// own; it makes frames for a sink as its clock moves; every verb that needs the console refuses; and a
// HostDriven system owns one from construction and produces from it.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "examples/common.h"
#include "mock_platform.h"  // test::CaptureAudioSink
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;
using Access = detail::AudioSystemTestAccess;

SnesBackend audioUnit() {
    SnesBackend backend;
    backend.hostAudioUnit();
    return backend;
}

TEST(SnesAudioUnit, ComesUpAsTheBootProgramWaitingOnThePorts) {
    SnesBackend unit = audioUnit();
    EXPECT_TRUE(SnesBackendTestAccess::audioUnitAlone(unit));
    EXPECT_FALSE(SnesBackendTestAccess::hosting(unit));  // no console was built
    EXPECT_EQ(unit.readMemory(snes::audioPort(0), 1), 0xAAu);
    EXPECT_EQ(unit.readMemory(snes::audioPort(1), 1), 0xBBu);
}

TEST(SnesAudioUnit, ThePlacesLandOnItByTheSameNames) {
    SnesBackend unit = audioUnit();

    unit.writeMemory(snes::audioRam(0x0500), 0xC3, 1);
    EXPECT_EQ(unit.readMemory(snes::audioRam(0x0500), 1), 0xC3u);
    EXPECT_EQ(SnesBackendTestAccess::audioUnitState(unit).ram[0x0500], 0xC3u);

    // A port keeps its two latches: the write reaches the sound CPU's side, the read is the console's.
    unit.writeMemory(snes::audioPort(2), 0x5A, 1);
    EXPECT_EQ(SnesBackendTestAccess::audioUnitState(unit).inputPorts[2], 0x5Au);
    EXPECT_NE(unit.readMemory(snes::audioPort(2), 1), 0x5Au);

    // A DSP write is the chip's own: a key-on arms the voices the next poll takes.
    unit.writeMemory(snes::dspRegister(0x4C), 0x05, 1);
    EXPECT_EQ(unit.readMemory(snes::dspRegister(0x4C), 1), 0x05u);
    EXPECT_EQ(SnesBackendTestAccess::audioUnitState(unit).dsp.internalKon, 0x05u);

    // Nothing on a bus it does not have.
    EXPECT_FALSE(unit.regionIsAddressable(snes::WorkRam));
    EXPECT_FALSE(unit.regionIsAddressable(snes::VideoRam));
    EXPECT_FALSE(unit.regionIsAddressable(MemoryRegion{.at = 0x008000, .size = 1}));
    EXPECT_TRUE(unit.regionIsAddressable(snes::AudioRam));
}

TEST(SnesAudioUnit, ItsClockIsItsOwnAndItMakesFramesAsItMoves) {
    SnesBackend unit = audioUnit();
    const std::optional<vm::MachineClock> clock = unit.clock();
    ASSERT_TRUE(clock.has_value());
    EXPECT_EQ(clock->hertzNumerator, 1'024'000u);
    EXPECT_EQ(clock->hertzDivisor, 1u);
    EXPECT_EQ(clock->cyclesPerFrame, 16'000u);

    std::size_t frames = 0;
    unit.enableAudio(32'000, [&frames](std::int16_t, std::int16_t) { ++frames; });
    unit.advanceClock(clock->cyclesPerFrame);  // 16'000 cycles: 500 DSP samples
    EXPECT_EQ(frames, 500u);
}

TEST(SnesAudioUnit, EveryVerbThatNeedsTheConsoleRefuses) {
    SnesBackend unit = audioUnit();
    EXPECT_FALSE(unit.takesButtons());
    EXPECT_FALSE(unit.keepsSaveData());
    EXPECT_THROW(unit.loadRom(snaggletooth::examples::loRomImage(1)), std::logic_error);
    EXPECT_THROW((void)unit.placeRoutine(std::vector<std::uint8_t>{0x60}, std::nullopt), std::logic_error);
    EXPECT_THROW((void)unit.assemble("        RTS\n"), std::logic_error);
    EXPECT_THROW(unit.armEscape(0x008000, false), std::logic_error);
    EXPECT_THROW(unit.armWatch(snes::AudioRam, true, false), std::logic_error);
    EXPECT_THROW(unit.setVideoEnabled(true), std::logic_error);
    EXPECT_THROW(unit.beginContinuous(0x008000), std::logic_error);

    // And the other way: a machine that holds a console is not made its audio unit.
    SnesBackend console;
    console.loadRom(snaggletooth::examples::loRomImage(1));
    EXPECT_THROW(console.hostAudioUnit(), std::logic_error);
}

TEST(SnesAudioUnit, AHostDrivenSystemOwnsOneFromConstructionAndProducesFromIt) {
    test::CaptureAudioSink sink;
    auto system = Access::makeManual(AudioKind::HostDriven, sink, VMPlatform::Snes, TimingProfile::Snes);

    EXPECT_TRUE(system->isPlaying());  // the unit is the system: running from its first step
    EXPECT_EQ(Access::voiceCount(*system), 1u);
    EXPECT_EQ(Access::framesPerStep(*system), 16'000u * 48'000u / 1'024'000u);  // one frame of its clock

    Access::step(*system);
    EXPECT_FALSE(sink.drain(system->audioStats().framesBuffered).empty());
}

TEST(SnesAudioUnit, TheGameBoyHasNoAudioUnitOfItsOwn) {
    test::CaptureAudioSink sink;
    EXPECT_THROW((void)Access::makeManual(AudioKind::HostDriven, sink), std::logic_error);
}

}  // namespace
}  // namespace retropp
