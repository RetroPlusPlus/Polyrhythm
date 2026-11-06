// A SNES machine's whole state through its backend: taken where the machine has finished a frame,
// handed over as bytes, and put back so the frames and the sound that follow are the ones the first
// run produced.
//
// The demo cartridge is played with right held, so its sprite moves every frame, and its sound passes
// through the rate converter, whose history is part of what a sample depends on.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "examples/snes/cartridge/cartridge.h"
#include "retropp/snes.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

constexpr std::uint64_t kFrame = 357'366;

// A one-bank LoROM cartridge that rests where its reset lands. With a battery its header declares 8 KB
// of save (chipset low nibble $2, save-size code $03); without one it declares none.
std::vector<std::uint8_t> restingCartridge(bool battery) {
    const snaggletooth::assembler::Assembly program = snaggletooth::assembler::assembleCpu65816(
        "        ORG $00:8000\n"
        "here:   BRA here\n",
        "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    if (battery) {
        rom[0x7FC0u + 0x16] = 0x02u;  // chipset: RAM + battery
        rom[0x7FC0u + 0x18] = 0x03u;  // save-size code: 8 KB
    }
    return rom;
}

// The word that holds right on port one, with port two empty.
[[nodiscard]] std::uint64_t rightHeld() {
    constexpr snes::Ports kPorts{.one = snes::Buttons{.right = true}, .two = std::nullopt};
    return static_cast<GuestButtons>(kPorts).held;
}

// The demo cartridge, hosted, keeping every frame it finishes and every sample it sounds. A backend is
// a third of a megabyte, so a case holds this on the heap.
struct Hosted {
    SnesBackend                            backend;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::int16_t>              samples;

    explicit Hosted(unsigned rate = 48'000) {
        backend.loadRom(examples::snes::demoCartridge());
        backend.bootHostedRom();
        backend.setFrameSink([this](std::span<const std::uint8_t> pixels, int, int, RasterPixelFormat,
                                    vm::FrameField) {
            frames.emplace_back(pixels.begin(), pixels.end());
        });
        backend.setVideoEnabled(true);
        listenAt(rate);
    }

    void listenAt(unsigned rate) {
        backend.enableAudio(rate, [this](std::int16_t left, std::int16_t right) {
            samples.push_back(left);
            samples.push_back(right);
        });
    }

    void clearSinks() {
        frames.clear();
        samples.clear();
    }

    void runFrames(int count) {
        for (int i = 0; i < count; ++i) {
            backend.runForCycles(kFrame);
        }
    }
};

[[nodiscard]] std::uint64_t readRegister(SnesBackend& backend, snes::Reg reg) {
    return backend.readRegister(static_cast<std::uint16_t>(reg));
}

TEST(SnesMachineState, TheCoreHandsItsStateOverAndTheAudioUnitAloneHandsOverNone) {
    auto backend = std::make_unique<SnesBackend>();
    EXPECT_TRUE(backend->keepsState());

    backend->hostAudioUnit();
    EXPECT_FALSE(backend->keepsState());
    EXPECT_THROW((void)backend->readState(), std::logic_error);
}

TEST(SnesMachineState, AStateTakenTwiceIsTheSameBytes) {
    auto machine = std::make_unique<Hosted>();
    machine->backend.runForCycles(kFrame * 20 + 130'000);

    const std::vector<std::uint8_t> first = machine->backend.readState();
    EXPECT_EQ(machine->backend.readState(), first);
}

TEST(SnesMachineState, ARestoreGivesTheSameBytesBack) {
    auto machine = std::make_unique<Hosted>();
    machine->backend.runForCycles(kFrame * 20 + 130'000);
    const std::vector<std::uint8_t> state = machine->backend.readState();

    machine->backend.runForCycles(kFrame * 5 + 90'000);

    EXPECT_TRUE(machine->backend.writeState(state));
    EXPECT_EQ(machine->backend.readState(), state);
}

TEST(SnesMachineState, AFramePartDrawnIsFinishedBeforeTheStateIsTaken) {
    auto machine = std::make_unique<Hosted>();
    machine->backend.runForCycles(kFrame * 20 + 130'000);

    const snaggletooth::SnesState& beam = SnesBackendTestAccess::state(machine->backend);
    ASSERT_GE(beam.vpos, 1u);
    ASSERT_LE(beam.vpos, 224u);
    ASSERT_FALSE(beam.inVblank);

    (void)machine->backend.readState();

    EXPECT_TRUE(beam.inVblank || beam.vpos == 0);
}

TEST(SnesMachineState, TakingAStateLeavesTheMachineOnItsGrid) {
    auto taken = std::make_unique<Hosted>();
    auto left  = std::make_unique<Hosted>();
    taken->backend.runForCycles(kFrame * 20 + 130'000);
    left->backend.runForCycles(kFrame * 20 + 130'000);

    (void)taken->backend.readState();

    taken->runFrames(3);
    left->runFrames(3);
    EXPECT_EQ(taken->backend.readState(), left->backend.readState());
    EXPECT_TRUE(SnesBackendTestAccess::state(taken->backend) == SnesBackendTestAccess::state(left->backend));
}

TEST(SnesMachineState, ARestoredMachineDrawsAndSoundsWhatTheOriginalDid) {
    auto machine = std::make_unique<Hosted>();
    machine->backend.setButtons(rightHeld());
    // Line 146: past the rows the sprite moves on (104 to 134), so a state taken here as it stands would
    // hand the frame after it those rows from the run before.
    machine->backend.runForCycles(kFrame * 60 + 200'000);
    const std::vector<std::uint8_t> state = machine->backend.readState();

    machine->clearSinks();
    machine->runFrames(8);
    ASSERT_GE(machine->frames.size(), 6u);
    ASSERT_GE(machine->samples.size(), 6'000u);
    const std::vector<std::vector<std::uint8_t>> frames(machine->frames.begin(), machine->frames.begin() + 6);
    const std::vector<std::int16_t> samples(machine->samples.begin(), machine->samples.begin() + 6'000);
    const std::vector<std::uint8_t> landed = machine->backend.readState();
    EXPECT_NE(frames.front(), frames.back());  // the sprite moves

    machine->backend.runForCycles(kFrame * 3 + 119'000);
    ASSERT_TRUE(machine->backend.writeState(state));
    machine->backend.setButtons(rightHeld());

    machine->clearSinks();
    machine->runFrames(8);
    ASSERT_GE(machine->frames.size(), 6u);
    ASSERT_GE(machine->samples.size(), 6'000u);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(machine->frames[i], frames[i]) << "frame " << i;
    }
    EXPECT_TRUE(std::equal(samples.begin(), samples.end(), machine->samples.begin()));
    EXPECT_EQ(machine->backend.readState(), landed);
}

TEST(SnesMachineState, BytesThatAreNoStateAreRefusedAndTheMachineIsAsItWas) {
    auto machine = std::make_unique<Hosted>();
    machine->backend.runForCycles(kFrame * 20 + 130'000);
    const std::vector<std::uint8_t> state = machine->backend.readState();

    std::vector<std::uint8_t> shortened = state;
    shortened.pop_back();
    std::vector<std::uint8_t> padded = state;
    padded.push_back(0);
    const std::vector<std::uint8_t> noise(537, 0xA5);

    EXPECT_FALSE(machine->backend.writeState(shortened));
    EXPECT_FALSE(machine->backend.writeState(padded));
    EXPECT_FALSE(machine->backend.writeState(noise));
    EXPECT_EQ(machine->backend.readState(), state);
}

TEST(SnesMachineState, AStateOfAMachineKeepingAnotherSaveIsRefused) {
    auto keeping = std::make_unique<SnesBackend>();
    keeping->loadRom(restingCartridge(/*battery=*/true));
    ASSERT_GT(keeping->saveDataSize(), 0u);

    auto keepingNothing = std::make_unique<SnesBackend>();
    keepingNothing->loadRom(restingCartridge(/*battery=*/false));
    ASSERT_EQ(keepingNothing->saveDataSize(), 0u);

    EXPECT_FALSE(keepingNothing->writeState(keeping->readState()));
}

TEST(SnesMachineState, AStateTakenThroughAnotherRateLeavesTheConverterAsItIs) {
    auto machine = std::make_unique<Hosted>();
    machine->runFrames(20);
    const std::vector<std::uint8_t> state = machine->backend.readState();

    machine->listenAt(44'100);
    ASSERT_TRUE(machine->backend.writeState(state));

    machine->clearSinks();
    machine->runFrames(1);
    // 44'100 Hz over a frame of 60.0988 Hz is 733.8 frames of sound; 48'000 Hz would be 798.7.
    const std::size_t produced = machine->samples.size() / 2;
    EXPECT_TRUE(produced == 733u || produced == 734u) << produced;
}

TEST(SnesMachineState, ArmedEscapesStayThroughARestore) {
    auto machine = std::make_unique<Hosted>();
    int  reached = 0;
    machine->backend.setEscapeSink([&reached](std::uint32_t) { ++reached; });
    machine->backend.armEscape(0x0083A0, /*replacesRoutine=*/false);  // `report`, called once a frame
    machine->runFrames(20);

    const std::vector<std::uint8_t> state = machine->backend.readState();

    const int before = reached;
    machine->runFrames(1);
    const int afterOne = reached;
    EXPECT_GT(afterOne, before);

    ASSERT_TRUE(machine->backend.writeState(state));
    machine->runFrames(1);
    EXPECT_GT(reached, afterOne);
}

TEST(SnesMachineState, AMachineHoldingPlacedRoutinesIdlesToItsFramesEnd) {
    auto backend = std::make_unique<SnesBackend>();
    constexpr std::uint8_t kIncrement[] = {0x1A, 0x60};  // INC A ; RTS
    (void)backend->placeRoutine(kIncrement, std::nullopt);

    backend->advanceClock(kFrame + 130'000);

    const snaggletooth::SnesState& beam = SnesBackendTestAccess::state(*backend);
    ASSERT_GE(beam.vpos, 1u);
    ASSERT_LE(beam.vpos, 224u);

    const std::uint64_t pc = readRegister(*backend, snes::Reg::PC);
    const std::uint64_t s  = readRegister(*backend, snes::Reg::S);
    const std::uint64_t c  = readRegister(*backend, snes::Reg::C);

    (void)backend->readState();

    EXPECT_EQ(readRegister(*backend, snes::Reg::PC), pc);
    EXPECT_EQ(readRegister(*backend, snes::Reg::S), s);
    EXPECT_EQ(readRegister(*backend, snes::Reg::C), c);
    EXPECT_TRUE(beam.inVblank || beam.vpos == 0);
}

}  // namespace
}  // namespace retropp
