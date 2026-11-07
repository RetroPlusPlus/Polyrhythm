// A Game Boy machine's whole state through its backend: taken where the machine has finished a frame,
// handed over as bytes, and put back so the frames and the samples that follow are the ones the first
// run produced.
//
// The cartridge is authored here, byte for byte. Its picture differs every frame and its tone steps
// with it, so a machine restored to the wrong moment shows in the first frame compared.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"
#include "src/vm/gameboy/sameboy_backend.h"
#include "tests/authored_cartridge.h"

extern "C" {
std::size_t retropp_sameboy_output_stage_size(void);
void        GB_random_seed(std::uint64_t seed);
}

namespace retropp {
namespace {

using vm::ConsoleModel;
using vm::SameBoyBackend;

constexpr std::uint64_t kFrame = 70'224;

// Stripes that move a pixel a frame, the count spelled along the top row of every tile so no two
// frames are the same picture, and a tone that steps with it.
constexpr std::string_view kGameSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ei
    halt                ; the first vertical blank: video RAM takes writes
    nop
    ld hl, $8000        ; tile 0, which every cell of the blank map shows
    ld b, $10
stripes:
    ld a, $F0           ; four dark columns, four light
    ld [hl+], a
    dec b
    jr nz, stripes
    ld a, $80
    ldh [$FF26], a      ; the sound chip on
    ld a, $FF
    ldh [$FF25], a      ; every channel to both sides
    ld a, $77
    ldh [$FF24], a      ; full volume
    ld a, $80
    ldh [$FF11], a      ; a square wave, half duty
    ld a, $F0
    ldh [$FF12], a      ; loud, and held
    ld a, $87
    ldh [$FF14], a      ; sounding
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ldh [$FF43], a      ; the stripes move a pixel
    ld [$8000], a       ; and the top row of every tile spells the count
    ldh [$FF13], a      ; the tone steps with it
    jr loop
)";

// A cartridge whose entry jumps to the program at $0150; the VBlank vector holds reti.
std::vector<std::uint8_t> gameCartridge(const SameBoyBackend& assembler) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(testing::kSmallestCartridge);
    rom[0x143] = 0x80;                                        // a Game Boy Color cartridge
    rom[0x040] = 0xD9;                                        // VBlank vector: reti
    rom[0x100] = 0x00;                                        // entry: nop
    rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;  // jp $0150
    const std::vector<std::uint8_t> code = assembler.assemble(kGameSource).bytes;
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

// The cartridge hosted and booted, keeping every frame it finishes and every sample it sounds.
struct Hosted {
    SameBoyBackend                         backend;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::int16_t>              samples;

    explicit Hosted(ConsoleModel model = ConsoleModel::GameBoyColor) : backend(model) {
        backend.loadRom(gameCartridge(backend));
        backend.bootHostedRom();
        backend.setFrameSink([this](std::span<const std::uint8_t> pixels, int, int, RasterPixelFormat,
                                    vm::FrameField) {
            frames.emplace_back(pixels.begin(), pixels.end());
        });
        backend.setVideoEnabled(true);
        backend.enableAudio(48'000, [this](std::int16_t left, std::int16_t right) {
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

[[nodiscard]] std::uint64_t readRegister(SameBoyBackend& backend, gb::Reg reg) {
    return backend.readRegister(static_cast<std::uint16_t>(reg));
}

// A cartridge of `type` running `source`, booted on a Game Boy Color with no sink attached.
void hostProgram(SameBoyBackend& backend, std::string_view source, std::uint8_t type, std::uint8_t ramSize) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(testing::kSmallestCartridge, type, ramSize);
    rom[0x040] = 0xD9;                                        // VBlank vector: reti
    rom[0x100] = 0x00;                                        // entry: nop
    rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;  // jp $0150
    const std::vector<std::uint8_t> code = backend.assemble(source).bytes;
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    backend.loadRom(rom);
    backend.bootHostedRom();
}

// Each vertical blank, latch the cartridge's real-time clock and copy its seconds to $FF81.
constexpr std::string_view kClockReader = R"(
    ld a, $01
    ldh [$FFFF], a
    ld a, $0A
    ld [$0000], a       ; the clock's registers answer
    xor a
    ldh [$FF0F], a
    ei
loop:
    halt
    nop
    xor a
    ld [$6000], a
    inc a
    ld [$6000], a       ; latch
    ld a, $08
    ld [$4000], a       ; the seconds
    ld a, [$A000]
    ldh [$FF81], a
    jr loop
)";

// Takes a picture over and over, copying a row of it to $FF81 each time and counting the shots at $FF82.
// The camera runs only while the CPU does, so this polls where the others halt.
constexpr std::string_view kCameraReader = R"(
    ld a, $0A
    ld [$0000], a       ; the cartridge's memory answers
    ld a, $10
    ld [$4000], a       ; the camera's registers
    ld [$A002], a       ; an exposure that keeps each pixel's value
    ld hl, $A006
    ld b, $30
    ld a, $80
thresholds:
    ld [hl+], a         ; dark below the middle, light above it
    dec b
    jr nz, thresholds
    xor a
    ldh [$FF82], a
    ld a, $01
    ld [$A000], a       ; shoot
loop:
    ld a, [$A000]
    and $01
    jr nz, loop         ; the camera is busy
    xor a
    ld [$4000], a       ; the picture
    ld a, [$A123]
    ldh [$FF81], a
    ldh a, [$FF82]
    inc a
    ldh [$FF82], a
    ld a, $10
    ld [$4000], a       ; the camera's registers
    ld a, $01
    ld [$A000], a       ; shoot
    jr loop
)";

constexpr std::uint8_t kClockCartridge  = 0x10;  // MBC3 with a clock, RAM and a battery
constexpr std::uint8_t kCameraCartridge = 0xFC;
constexpr std::uint32_t kReading        = 0xFF81;

TEST(SameBoyMachineState, ACartridgesClockCarriesOnFromTheStateHoweverLongItWasKept) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    hostProgram(backend, kClockReader, kClockCartridge, testing::kRam8K);
    for (int i = 0; i < 30; ++i) {
        backend.runForCycles(kFrame);
    }
    const std::vector<std::uint8_t> state = backend.readState();

    // Two guest seconds, so the clock has ticked under the machine's own time.
    for (int i = 0; i < 120; ++i) {
        backend.runForCycles(kFrame);
    }
    const std::uint64_t             reading = backend.readMemory(kReading, 1);
    const std::vector<std::uint8_t> landed  = backend.readState();
    EXPECT_GE(reading, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds{2'100});  // the wall clock moves on

    ASSERT_TRUE(backend.writeState(state));
    for (int i = 0; i < 120; ++i) {
        backend.runForCycles(kFrame);
    }
    EXPECT_EQ(backend.readMemory(kReading, 1), reading);
    EXPECT_EQ(backend.readState(), landed);

    // A machine that has never handed a state over carries on from it the same way.
    SameBoyBackend fresh{ConsoleModel::GameBoyColor};
    hostProgram(fresh, kClockReader, kClockCartridge, testing::kRam8K);
    ASSERT_TRUE(fresh.writeState(state));
    for (int i = 0; i < 120; ++i) {
        fresh.runForCycles(kFrame);
    }
    EXPECT_EQ(fresh.readMemory(kReading, 1), reading);
    EXPECT_EQ(fresh.readState(), landed);
}

TEST(SameBoyMachineState, ACameraCartridgeSeesThePictureTheFirstRunSaw) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    hostProgram(backend, kCameraReader, kCameraCartridge, testing::kRam8K);
    for (int i = 0; i < 30; ++i) {
        backend.runForCycles(kFrame);
    }
    const std::vector<std::uint8_t> state = backend.readState();

    std::vector<std::uint64_t> readings;
    for (int i = 0; i < 30; ++i) {
        backend.runForCycles(kFrame);
        readings.push_back(backend.readMemory(kReading, 1));
    }

    EXPECT_GT(backend.readMemory(0xFF82, 1), 2u);  // the shots land while the case watches

    ASSERT_TRUE(backend.writeState(state));
    for (int i = 0; i < 30; ++i) {
        backend.runForCycles(kFrame);
        EXPECT_EQ(backend.readMemory(kReading, 1), readings[static_cast<std::size_t>(i)]) << "frame " << i;
    }
}

TEST(SameBoyMachineState, AKeyStillSettlingKeepsSettlingAfterARestore) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20);
    (void)machine.backend.readState();  // the machine stands where a frame has finished

    constexpr std::uint64_t kRight = 1u << 0;
    machine.backend.setButtons(kRight);
    const std::vector<std::uint8_t> state = machine.backend.readState();

    machine.runFrames(2);
    const std::vector<std::uint8_t> landed = machine.backend.readState();

    ASSERT_TRUE(machine.backend.writeState(state));
    machine.backend.setButtons(kRight);
    machine.runFrames(2);
    EXPECT_EQ(machine.backend.readState(), landed);
}

TEST(SameBoyMachineState, TheCoreHandsItsStateOver) {
    EXPECT_TRUE(SameBoyBackend{ConsoleModel::GameBoy}.keepsState());
    EXPECT_TRUE(SameBoyBackend{ConsoleModel::GameBoyColor}.keepsState());
}

TEST(SameBoyMachineState, AStateTakenTwiceIsTheSameBytes) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20 + 20'000);

    const std::vector<std::uint8_t> first = machine.backend.readState();
    const std::size_t               frames = machine.frames.size();
    const std::vector<std::uint8_t> second = machine.backend.readState();

    EXPECT_EQ(first, second);
    EXPECT_EQ(machine.frames.size(), frames);  // the second call finished no frame
}

TEST(SameBoyMachineState, ARestoreGivesTheSameBytesBack) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20 + 20'000);
    const std::vector<std::uint8_t> state = machine.backend.readState();

    machine.backend.runForCycles(kFrame * 5 + 31'000);

    EXPECT_TRUE(machine.backend.writeState(state));
    EXPECT_EQ(machine.backend.readState(), state);
}

TEST(SameBoyMachineState, AFramePartDrawnIsFinishedBeforeTheStateIsTaken) {
    bool anyFinished = false;
    for (const std::uint64_t offset : {std::uint64_t{10'000}, std::uint64_t{30'000}, std::uint64_t{50'000}}) {
        Hosted machine;
        machine.backend.runForCycles(kFrame * 20 + offset);

        const std::size_t               before = machine.frames.size();
        const std::vector<std::uint8_t> first  = machine.backend.readState();
        const std::size_t               after  = machine.frames.size();
        EXPECT_LE(after - before, 1u) << "at " << offset;
        anyFinished = anyFinished || after != before;

        const std::vector<std::uint8_t> second = machine.backend.readState();
        EXPECT_EQ(machine.frames.size(), after) << "at " << offset;
        EXPECT_EQ(first, second) << "at " << offset;
    }
    EXPECT_TRUE(anyFinished);
}

TEST(SameBoyMachineState, TakingAStateLeavesTheMachineOnItsGrid) {
    // Two machines are identical at power-on under the same seed of the generator the core shares, and
    // when both are built within one second of the wall clock, which the core's state records. A byte
    // that differs later is one the state moved.
    std::unique_ptr<Hosted> taken;
    std::unique_ptr<Hosted> left;
    std::time_t             second = 0;
    do {
        second = std::time(nullptr);
        GB_random_seed(0x5AFE);
        taken = std::make_unique<Hosted>();
        GB_random_seed(0x5AFE);
        left = std::make_unique<Hosted>();
    } while (std::time(nullptr) != second);
    taken->backend.runForCycles(kFrame * 20 + 20'000);
    left->backend.runForCycles(kFrame * 20 + 20'000);

    (void)taken->backend.readState();

    taken->runFrames(3);
    left->runFrames(3);
    EXPECT_EQ(taken->backend.readState(), left->backend.readState());
}

TEST(SameBoyMachineState, ARestoredMachineDrawsAndSoundsWhatTheOriginalDid) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20 + 20'000);
    const std::vector<std::uint8_t> state = machine.backend.readState();

    machine.clearSinks();
    machine.runFrames(10);  // the first is short by what the state ran ahead
    ASSERT_GE(machine.frames.size(), 6u);
    ASSERT_GE(machine.samples.size(), 12'000u);
    const std::vector<std::vector<std::uint8_t>> frames(machine.frames.begin(), machine.frames.begin() + 6);
    const std::vector<std::int16_t> samples(machine.samples.begin(), machine.samples.begin() + 12'000);
    const std::vector<std::uint8_t> landed = machine.backend.readState();
    EXPECT_NE(frames.front(), frames.back());  // the picture moves

    // An odd count ends mid-way through an instruction's cycles, so the run left behind owes the machine
    // a different overshoot from the one the state carries.
    machine.backend.runForCycles(kFrame * 3 + 23'001);
    ASSERT_TRUE(machine.backend.writeState(state));

    machine.clearSinks();
    machine.runFrames(10);
    ASSERT_GE(machine.frames.size(), 6u);
    ASSERT_GE(machine.samples.size(), 12'000u);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(machine.frames[i], frames[i]) << "frame " << i;
    }
    EXPECT_TRUE(std::equal(samples.begin(), samples.end(), machine.samples.begin()));
    EXPECT_EQ(machine.backend.readState(), landed);
}

TEST(SameBoyMachineState, BytesOfAnotherLengthAreRefusedAndTheMachineIsAsItWas) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20 + 20'000);
    const std::vector<std::uint8_t> state = machine.backend.readState();

    std::vector<std::uint8_t> shortened = state;
    shortened.pop_back();
    std::vector<std::uint8_t> padded = state;
    padded.push_back(0);

    EXPECT_FALSE(machine.backend.writeState(shortened));
    EXPECT_FALSE(machine.backend.writeState(padded));
    EXPECT_FALSE(machine.backend.writeState({}));
    EXPECT_EQ(machine.backend.readState(), state);
}

TEST(SameBoyMachineState, AnotherLayoutOfTheCoresBytesIsRefused) {
    Hosted machine;
    machine.backend.runForCycles(kFrame * 20 + 20'000);
    const std::vector<std::uint8_t> state = machine.backend.readState();

    std::vector<std::uint8_t> other = state;
    other[16 + retropp_sameboy_output_stage_size() + 4] ^= 0xFF;  // the low byte of the core's layout version

    EXPECT_FALSE(machine.backend.writeState(other));
    EXPECT_EQ(machine.backend.readState(), state);
}

TEST(SameBoyMachineState, TheOtherModelsStateIsRefused) {
    Hosted gameBoy{ConsoleModel::GameBoy};
    gameBoy.runFrames(2);

    Hosted color{ConsoleModel::GameBoyColor};
    EXPECT_FALSE(color.backend.writeState(gameBoy.backend.readState()));
}

TEST(SameBoyMachineState, AReplacingEscapeInWorkRamIsNoPartOfAState) {
    Hosted machine;
    machine.backend.writeMemory(0xC100, 0x3C, 1);
    machine.backend.armEscape(0xC100, /*replacesRoutine=*/true);
    EXPECT_EQ(machine.backend.readMemory(0xC100, 1), 0xC9u);

    const std::vector<std::uint8_t> state = machine.backend.readState();
    EXPECT_EQ(machine.backend.readMemory(0xC100, 1), 0xC9u);  // still armed once the state is taken

    // Restored with none armed, the machine holds the byte the escape displaced.
    machine.backend.disarmEscape(0xC100);
    EXPECT_TRUE(machine.backend.writeState(state));
    EXPECT_EQ(machine.backend.readMemory(0xC100, 1), 0x3Cu);

    // Restored with one armed, the return stands, and disarming finds the state's byte beneath it.
    machine.backend.armEscape(0xC100, /*replacesRoutine=*/true);
    EXPECT_TRUE(machine.backend.writeState(state));
    EXPECT_EQ(machine.backend.readMemory(0xC100, 1), 0xC9u);
    machine.backend.disarmEscape(0xC100);
    EXPECT_EQ(machine.backend.readMemory(0xC100, 1), 0x3Cu);
}

TEST(SameBoyMachineState, AMachineHoldingPlacedRoutinesIdlesToItsFramesEnd) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};

    // ld a,$91 ; ldh [$40],a ; ret — the routine switches the display on.
    constexpr std::uint8_t kDisplayOn[] = {0x3E, 0x91, 0xE0, 0x40, 0xC9};
    backend.beginCall(backend.placeRoutine(kDisplayOn, std::nullopt));
    backend.run();
    backend.advanceClock(kFrame + 30'000);

    const std::uint64_t pc = readRegister(backend, gb::Reg::PC);
    const std::uint64_t sp = readRegister(backend, gb::Reg::SP);
    const std::uint64_t af = readRegister(backend, gb::Reg::AF);

    const std::vector<std::uint8_t> first = backend.readState();

    EXPECT_EQ(readRegister(backend, gb::Reg::PC), pc);
    EXPECT_EQ(readRegister(backend, gb::Reg::SP), sp);
    EXPECT_EQ(readRegister(backend, gb::Reg::AF), af);
    EXPECT_EQ(backend.readState(), first);
}

}  // namespace
}  // namespace retropp
