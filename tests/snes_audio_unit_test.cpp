// The SNES audio unit alone: the machine an AudioSystem::SNES of the HostDriven kind hosts — the sound
// CPU, its RAM and the sound chip, with no 5A22 and no picture chip built around them.
//
// The cases: the unit comes up in its boot program, which posts the ready bytes, answers a kick and is
// re-entered by a reset; the three audio places land on it by the same names and the comm ports keep
// their two latches; a DSP write is the chip's own; its clock is its
// own; it makes frames for a sink as its clock moves; every verb that needs the console refuses; a
// HostDriven system owns one from construction and produces from it; the game reaches it through the
// system's own read / write, which cross to the unit's thread and come back answered; stop() leaves it
// running; and a system of any other kind has no unit to reach.

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
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

TEST(SnesAudioUnit, ComesUpInItsBootProgramWhichPostsTheReadyBytesAndAnswersAKick) {
    SnesBackend unit = audioUnit();
    EXPECT_TRUE(SnesBackendTestAccess::audioUnitAlone(unit));
    EXPECT_FALSE(SnesBackendTestAccess::hosting(unit));  // no console was built

    // The sound CPU runs the upload stub from the boot window: a few cycles in, the ready bytes are posted.
    unit.advanceClock(64);
    EXPECT_EQ(unit.readMemory(snes::audioPort(0), 1), 0xAAu);
    EXPECT_EQ(unit.readMemory(snes::audioPort(1), 1), 0xBBu);

    // A kick — a destination on ports 2-3, a transfer on port 1, $CC on port 0 — is echoed on port 0.
    unit.writeMemory(snes::audioPort(2), 0x0200, 2);
    unit.writeMemory(snes::audioPort(1), 0x01, 1);
    unit.writeMemory(snes::audioPort(0), 0xCC, 1);
    unit.advanceClock(256);
    EXPECT_EQ(unit.readMemory(snes::audioPort(0), 1), 0xCCu);

    // A reset puts the sound CPU back at the stub's entry: the ports clear, then the ready bytes return.
    unit.reset();
    EXPECT_EQ(unit.readMemory(snes::audioPort(0), 1), 0x00u);
    unit.advanceClock(64);
    EXPECT_EQ(unit.readMemory(snes::audioPort(0), 1), 0xAAu);
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

// A place: `size` bytes at an address a snes:: helper names.
MemoryRegion at(std::uint32_t address, std::uint32_t size = 1) { return MemoryRegion{.at = address, .size = size}; }

TEST(SnesAudioUnit, TheGameDrivesItThroughTheSystemsOwnPlaceVerbs) {
    test::CaptureAudioSink sink;
    auto system = Access::makeManual(AudioKind::HostDriven, sink, VMPlatform::Snes, TimingProfile::Snes);

    // The boot program's greeting, on the ports, as the console's side reads it — once the unit has run.
    Access::step(*system);
    EXPECT_EQ(system->read(at(snes::audioPort(0), 2)), (std::vector<std::uint8_t>{0xAA, 0xBB}));

    // A write lands and the read that follows it sees it: a DSP register, and a byte of audio RAM.
    system->write(at(snes::dspRegister(0x0C)), std::vector<std::uint8_t>{0x60});
    EXPECT_EQ(system->read(at(snes::dspRegister(0x0C))), (std::vector<std::uint8_t>{0x60}));
    system->write(at(snes::audioRam(0x0500), 2), std::vector<std::uint8_t>{0xC3, 0x01});
    EXPECT_EQ(system->read(at(snes::audioRam(0x0500), 2)), (std::vector<std::uint8_t>{0xC3, 0x01}));

    // The DSP write is the chip's own: a key-on arms the voices.
    system->write(at(snes::dspRegister(0x4C)), std::vector<std::uint8_t>{0x05});
    EXPECT_EQ(system->read(at(snes::dspRegister(0x4C))), (std::vector<std::uint8_t>{0x05}));

    // A place the unit does not have, and a byte count that is not one entry, throw at the call.
    EXPECT_THROW((void)system->read(snes::WorkRam), std::out_of_range);
    EXPECT_THROW(system->write(at(snes::dspRegister(0x0C)), std::vector<std::uint8_t>{0x60, 0x60}),
                 std::invalid_argument);

    // stop() silences cued voices; the unit is the system and stays.
    system->stop();
    Access::step(*system);
    EXPECT_EQ(Access::voiceCount(*system), 1u);
    EXPECT_TRUE(system->isPlaying());
    EXPECT_EQ(system->read(at(snes::dspRegister(0x0C))), (std::vector<std::uint8_t>{0x60}));
}

TEST(SnesAudioUnit, OnItsOwnThreadTheUnitRunsByItselfAndAnswersTheGame) {
    // The public construction: a threaded system, the unit on a thread of its own, on its own clock.
    test::CaptureAudioSink sink;
    AudioSystem::SNES      system{AudioKind::HostDriven, sink};

    // A write crosses to the unit's thread and the read that follows comes back with it.
    system.write(at(snes::dspRegister(0x0C)), std::vector<std::uint8_t>{0x60});
    EXPECT_EQ(system.read(at(snes::dspRegister(0x0C))), (std::vector<std::uint8_t>{0x60}));

    // Nobody pulls the sink, and the unit still runs: its boot program posts the ready bytes and its
    // frames reach the system's output, on the unit's own clock. Bounded waits — the claim is that each
    // arrives, not when.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto ready    = std::vector<std::uint8_t>{0xAA, 0xBB};
    while (system.read(at(snes::audioPort(0), 2)) != ready && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(system.read(at(snes::audioPort(0), 2)), ready);
    while (system.audioStats().framesBuffered == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(system.audioStats().framesBuffered, 0u);
    EXPECT_FALSE(sink.drain(64).empty());
}

TEST(SnesAudioUnit, ASystemOfAnyOtherKindHasNoUnitToReach) {
    test::CaptureAudioSink sink;
    auto chiptune = Access::makeManual(AudioKind::Chiptune, sink, VMPlatform::Snes, TimingProfile::Snes);
    EXPECT_THROW((void)chiptune->read(at(snes::audioPort(0))), std::logic_error);
    EXPECT_THROW(chiptune->write(at(snes::audioPort(0)), std::vector<std::uint8_t>{0xCC}), std::logic_error);
}

TEST(SnesAudioUnit, TheGameBoyHasNoAudioUnitOfItsOwn) {
    test::CaptureAudioSink sink;
    EXPECT_THROW((void)Access::makeManual(AudioKind::HostDriven, sink), std::logic_error);
}

}  // namespace
}  // namespace retropp
