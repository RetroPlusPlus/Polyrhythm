// A machine's whole state through the public surface: where state() and restore() land, what the host
// layer carries beside the core's bytes, and what restore refuses.
//
// The first half runs on MockVmBackend, whose whole memory is its state, so the host's framing, its
// refusals and the pictures it carries are pinned with no console core underneath. The second half runs
// the real core over authored cartridges, because the fractions of a cycle, a restore before the first
// run, the landing on the same bytes and the battery file only mean something on a machine that runs.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/guest_buttons.h"
#include "retropp/guest_escape.h"
#include "retropp/memory_region.h"
#include "retropp/raster_content.h"
#include "retropp/vm.h"
#include "src/vm/vm_backend.h"
#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"
#include "tests/mock_vm_backend.h"

namespace retropp {
namespace {

using testing::MockVmBackend;
using vm::FrameField;
using vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for reading what crossed the seam.
struct MockedVm {
    Vm             machine;
    MockVmBackend* mock = nullptr;

    explicit MockedVm(VMPlatform platform = VMPlatform::GameBoy) : machine{platform} {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

// ── Authored cartridges ─────────────────────────────────────────────────────────────────────────

// A cartridge whose entry jumps to `source` assembled at $0150. The VBlank vector returns, and a lone
// ret sits past the CGB overlay window so code that wanders returns instead of running away.
std::vector<std::uint8_t> cartridgeRunning(Vm& assembler, std::string_view source,
                                           std::uint8_t cartridgeType = 0x00, std::uint8_t ramSizeByte = 0x00) {
    std::vector<std::uint8_t> rom =
        testing::authorCartridge(testing::kSmallestCartridge, cartridgeType, ramSizeByte);
    rom[0x143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x040] = 0xD9;  // VBlank: reti
    rom[0x100] = 0x00;  // entry: nop
    rom[0x101] = 0xC3;
    rom[0x102] = 0x50;
    rom[0x103] = 0x01;  // jp $0150
    rom[0x900] = 0xC9;  // ret past the CGB overlay window
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(source));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

// Counts frames: VBlank-only interrupts, halt in a loop, one high-RAM increment per wake.
constexpr std::string_view kFrameCounterSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter
    ldh [$FF0F], a      ; start counting from the NEXT VBlank, not a pending one
    ei
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a
    jr loop
)";

// Stripes that move a pixel a frame, the count spelled along the top row of every tile so no two frames
// are the same picture, and a tone that steps with it.
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

// What the real-core cases observe, declared once per machine before it runs.
struct Places {
    MemoryRegion hram;
    MemoryRegion wram;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(regions(
        region(&Places::hram, MemoryRegion{.at = 0xFF80, .size = 1, .count = 8}, "hram-observations"),
        region(&Places::wram, MemoryRegion{.at = 0xC000, .size = 1, .count = 4}, "wram-cells")));
}

// ── The refusals ────────────────────────────────────────────────────────────────────────────────

TEST(VmMachineState, ACoreWithNoModelRefusesAtTheCall) {
    MockedVm m;
    m.mock->keepsState_ = false;
    EXPECT_THROW(static_cast<void>(m.machine.state()), std::logic_error);
    EXPECT_THROW(static_cast<void>(m.machine.restore({})), std::logic_error);
    EXPECT_EQ(m.mock->statesRead, 0);
    EXPECT_EQ(m.mock->statesWritten, 0);
}

TEST(VmMachineState, BytesThatAreNoStateNeverReachTheCore) {
    MockedVm                        m;
    const std::vector<std::uint8_t> good = m.machine.state();

    EXPECT_FALSE(m.machine.restore({}));

    std::vector<std::uint8_t> cut(good.begin(), good.begin() + 59);
    EXPECT_FALSE(m.machine.restore(cut));

    for (const std::size_t at : {std::size_t{0}, std::size_t{4}, std::size_t{6}}) {
        std::vector<std::uint8_t> changed = good;
        changed[at] ^= 0xFF;
        EXPECT_FALSE(m.machine.restore(changed)) << "byte " << at;
    }

    std::vector<std::uint8_t> padded = good;
    padded.push_back(0x00);
    EXPECT_FALSE(m.machine.restore(padded));

    std::vector<std::uint8_t> shortByOne = good;
    shortByOne.pop_back();
    EXPECT_FALSE(m.machine.restore(shortByOne));

    EXPECT_EQ(m.mock->statesWritten, 0);

    MockedVm color{VMPlatform::GameBoyColor};
    EXPECT_FALSE(color.machine.restore(good));
    EXPECT_EQ(color.mock->statesWritten, 0);
}

// ── What travels, and what stays ────────────────────────────────────────────────────────────────

TEST(VmMachineState, AStateAndItsRestorePutTheMemoryBack) {
    MockedVm m;
    m.mock->bytes()[0x1234]             = 0x5A;
    const std::vector<std::uint8_t> state = m.machine.state();
    m.mock->bytes()[0x1234]             = 0x77;
    EXPECT_TRUE(m.machine.restore(state));
    EXPECT_EQ(m.mock->bytes()[0x1234], 0x5A);
}

TEST(VmMachineState, TheButtonsHeldStayTheGames) {
    MockedVm m;
    m.machine.buttons(GuestButtons{.held = 0x05});
    const std::vector<std::uint8_t> state = m.machine.state();
    m.machine.buttons(GuestButtons{.held = 0x0A});
    EXPECT_TRUE(m.machine.restore(state));
    EXPECT_EQ(m.mock->buttonsSet, (std::vector<std::uint64_t>{0x05, 0x0A, 0x0A}));
}

TEST(VmMachineState, AMachineNeverHandedButtonsIsHandedNone) {
    MockedVm                        m;
    const std::vector<std::uint8_t> state = m.machine.state();
    EXPECT_TRUE(m.machine.restore(state));
    EXPECT_TRUE(m.mock->buttonsSet.empty());
}

TEST(VmMachineState, ACallFromInsideAHandlerIsRefused) {
    MockedVm                        m;
    const std::vector<std::uint8_t> state = m.machine.state();
    int                             refused = 0;
    m.machine.registerEscapes(escapes(GuestEscape{
        .key = "asks", .at = 0xC310, .handler = [&](Vm& machine, std::uint32_t) {
            try {
                static_cast<void>(machine.state());
            } catch (const std::logic_error&) {
                ++refused;
            }
            try {
                static_cast<void>(machine.restore(state));
            } catch (const std::logic_error&) {
                ++refused;
            }
        }}));
    const std::vector<std::uint32_t> walk{0xC300, 0xC310};
    m.mock->walk(walk);
    EXPECT_EQ(refused, 2);
}

// ── The pictures the host carries ───────────────────────────────────────────────────────────────

TEST(VmMachineState, TheLastPictureTravels) {
    constexpr int kSide = 2;
    MockedVm      m;
    const std::vector<std::uint8_t> a(kSide * kSide * 4, 0x11);
    const std::vector<std::uint8_t> b(kSide * kSide * 4, 0x22);

    m.machine.video(true);
    m.mock->finishFrame(a, kSide, kSide);
    m.machine.advanceTick();
    const std::vector<std::uint8_t> state = m.machine.state();

    m.mock->finishFrame(b, kSide, kSide);
    m.machine.advanceTick();
    const RasterContent later = m.machine.video();
    ASSERT_EQ(later.pixels.size(), b.size());
    EXPECT_EQ(later.pixels[0], 0x22);
    const std::uint64_t generationOfB = later.generation;

    EXPECT_TRUE(m.machine.restore(state));
    const RasterContent back = m.machine.video();  // no tick between
    ASSERT_EQ(back.pixels.size(), a.size());
    EXPECT_TRUE(std::equal(back.pixels.begin(), back.pixels.end(), a.begin()));
    EXPECT_EQ(back.width, kSide);
    EXPECT_EQ(back.height, kSide);
    EXPECT_GT(back.generation, generationOfB);
}

// A field whose line y is every byte `base + y`, so a line landing at the wrong parity is caught.
std::vector<std::uint8_t> fieldOf(std::uint8_t base, int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int i = 0; i < width * 4; ++i) {
            pixels[static_cast<std::size_t>(y) * width * 4 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(base + y);
        }
    }
    return pixels;
}

TEST(VmMachineState, TheWovenPictureTravels) {
    constexpr int          kWidth = 4, kHeight = 2;
    constexpr std::uint8_t kEvenBefore = 10, kOddBefore = 20, kEvenAway = 30, kOddAway = 40, kEvenAfter = 50;
    MockedVm               m;
    m.machine.video(true, {.interlacing = {.on = true, .type = Weave::Straight}});

    m.mock->finishFrame(fieldOf(kEvenBefore, kWidth, kHeight), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOddBefore, kWidth, kHeight), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();
    const std::vector<std::uint8_t> state = m.machine.state();

    m.mock->finishFrame(fieldOf(kEvenAway, kWidth, kHeight), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOddAway, kWidth, kHeight), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    EXPECT_TRUE(m.machine.restore(state));
    m.mock->finishFrame(fieldOf(kEvenAfter, kWidth, kHeight), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    ASSERT_EQ(picture.height, 2 * kHeight);
    const auto lineByte = [&picture](int line) {
        return picture.pixels[static_cast<std::size_t>(line) * picture.width * 4];
    };
    EXPECT_EQ(lineByte(0), kEvenAfter);
    EXPECT_EQ(lineByte(1), kOddBefore);
    EXPECT_EQ(lineByte(2), kEvenAfter + 1);
    EXPECT_EQ(lineByte(3), kOddBefore + 1);
}

// ── On the real core ────────────────────────────────────────────────────────────────────────────

// A speed factor with a remainder, so the fraction the factor leaves behind is in play beside the one
// the tick leaves.
TEST(VmMachineState, TheTicksFractionsTravel) {
    using namespace std::chrono_literals;
    Vm::GBC machine;
    machine.speed(2, 3);
    for (int i = 0; i < 5; ++i) {
        static_cast<void>(VmTestAccess::tickBudget(machine, 16'666'667ns));
    }
    const std::vector<std::uint8_t> state = machine.state();

    std::vector<std::uint64_t> first;
    for (int i = 0; i < 7; ++i) {
        first.push_back(VmTestAccess::tickBudget(machine, 16'666'667ns));
    }
    EXPECT_TRUE(machine.restore(state));
    std::vector<std::uint64_t> second;
    for (int i = 0; i < 7; ++i) {
        second.push_back(VmTestAccess::tickBudget(machine, 16'666'667ns));
    }
    EXPECT_EQ(first, second);
}

TEST(VmMachineState, EveryVerbIssuedBeforeAStateIsInIt) {
    Vm::GBC machine;
    machine.hostRom(cartridgeRunning(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    VmTestAccess::stepOnce(machine);

    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x5A}, 0);
    const std::vector<std::uint8_t> state = machine.state();
    EXPECT_FALSE(state.empty());
    EXPECT_EQ(machine.read(places, &Places::wram, 0).at(0), 0x5A);
    machine.stop();
}

TEST(VmMachineState, ARestoreLandsAfterTheVerbsIssuedBeforeIt) {
    Vm::GBC machine;
    machine.hostRom(cartridgeRunning(machine, kFrameCounterSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    VmTestAccess::stepOnce(machine);

    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x5A}, 0);
    const std::vector<std::uint8_t> state = machine.state();
    VmTestAccess::stepOnce(machine);

    machine.write(places, &Places::wram, std::vector<std::uint8_t>{0x77}, 0);
    EXPECT_TRUE(machine.restore(state));
    EXPECT_EQ(machine.read(places, &Places::wram, 0).at(0), 0x5A);
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(machine.read(places, &Places::wram, 0).at(0), 0x5A);
    machine.stop();
}

TEST(VmMachineState, ARestoreBeforeTheFirstRunCarriesOnWithNoBoot) {
    std::vector<std::uint8_t> state;
    std::vector<std::uint8_t> image;
    std::uint8_t              counted = 0;
    {
        Vm::GBC first;
        image = cartridgeRunning(first, kFrameCounterSource);
        first.hostRom(image);
        const RegionMapId<Places> places = declarePlaces(first);
        VmTestAccess::runInline(first);
        for (int i = 0; i < 20; ++i) {
            VmTestAccess::stepOnce(first);
        }
        state   = first.state();
        counted = first.read(places, &Places::hram, 0).at(0);
        first.stop();
    }
    ASSERT_GE(counted, 10);

    Vm::GBC second;
    second.hostRom(image);
    const RegionMapId<Places> secondPlaces = declarePlaces(second);
    EXPECT_TRUE(second.restore(state));
    VmTestAccess::runInline(second);
    VmTestAccess::stepOnce(second);
    const std::uint8_t carriedOn = second.read(secondPlaces, &Places::hram, 0).at(0);
    EXPECT_GE(carriedOn, counted);
    EXPECT_LE(carriedOn, counted + 2);
    second.stop();

    Vm::GBC fresh;
    fresh.hostRom(image);
    const RegionMapId<Places> freshPlaces = declarePlaces(fresh);
    VmTestAccess::runInline(fresh);
    VmTestAccess::stepOnce(fresh);
    EXPECT_LE(fresh.read(freshPlaces, &Places::hram, 0).at(0), 2);
    fresh.stop();
}

TEST(VmMachineState, ATickAdvancedMachineLandsOnTheSameBytesAfterARestore) {
    Vm::GBC machine;
    machine.hostRom(cartridgeRunning(machine, kGameSource));
    machine.video(true);
    machine.audio(true, {.rate = 48'000, .onSample = [](std::int16_t, std::int16_t) {}});
    machine.run(Vm::Advance::OnTick);
    const auto ticks = [&machine](int count) {
        for (int i = 0; i < count; ++i) {
            machine.advanceTick();
        }
    };
    const auto picture = [&machine] {
        const RasterContent shown = machine.video();
        return std::vector<std::uint8_t>(shown.pixels.begin(), shown.pixels.end());
    };
    ticks(20);
    const std::vector<std::uint8_t> state      = machine.state();
    const std::vector<std::uint8_t> atTheState = picture();
    ticks(10);
    const std::vector<std::uint8_t> first = machine.state();
    const std::vector<std::uint8_t> away  = picture();
    ASSERT_NE(atTheState, away);  // the picture moved, so the comparison below can tell the two apart
    EXPECT_TRUE(machine.restore(state));
    EXPECT_EQ(picture(), atTheState);  // with no tick between: the state's picture, whole
    ticks(10);
    const std::vector<std::uint8_t> second = machine.state();
    EXPECT_EQ(first, second);
    machine.stop();
}

TEST(VmMachineState, AMachineOnItsOwnThreadAnswersFromTheGamesThread) {
    Vm::GBC machine;
    machine.hostRom(cartridgeRunning(machine, kFrameCounterSource));
    // Taken parked first, on this thread, so the call after run() has to tell this thread from the
    // machine's before the machine's thread has taken a step.
    EXPECT_FALSE(machine.state().empty());
    machine.run();
    const std::vector<std::uint8_t> state = machine.state();
    EXPECT_FALSE(state.empty());
    EXPECT_TRUE(machine.restore(state));
    EXPECT_FALSE(machine.state().empty());
    machine.stop();
}

// The audio function runs inside the machine's own step, on whichever thread steps it.
TEST(VmMachineState, ACallFromInsideAStepIsRefusedOnEitherClock) {
    const auto askingFrom = [](Vm& machine, std::atomic<bool>& asked, std::atomic<bool>& refused) {
        machine.audio(true, {.rate = 48'000, .onSample = [&machine, &asked, &refused](std::int16_t, std::int16_t) {
                                 if (asked.exchange(true)) {
                                     return;
                                 }
                                 try {
                                     static_cast<void>(machine.state());
                                 } catch (const std::logic_error&) {
                                     refused = true;
                                 }
                             }});
    };

    {
        std::atomic<bool> asked{false};
        std::atomic<bool> refused{false};
        Vm::GBC           machine;
        machine.hostRom(cartridgeRunning(machine, kFrameCounterSource));
        askingFrom(machine, asked, refused);
        machine.run(Vm::Advance::OnTick);
        machine.advanceTick();
        EXPECT_TRUE(refused.load());
        machine.stop();
    }
    {
        std::atomic<bool> asked{false};
        std::atomic<bool> refused{false};
        Vm::GBC           machine;
        machine.hostRom(cartridgeRunning(machine, kFrameCounterSource));
        askingFrom(machine, asked, refused);
        machine.run();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!refused.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(refused.load());
        machine.stop();
    }
}

TEST(VmMachineState, TheKeyedBatteryFileFollowsARestore) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "retropp-vm-state-test-battery";
    std::filesystem::remove_all(root);
    const auto firstByteOnDisk = [&root] {
        std::ifstream in(root / "VM" / "cartridge" / "battery.sav", std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        return bytes.empty() ? std::uint8_t{0} : static_cast<std::uint8_t>(bytes[0]);
    };
    {
        // Unlock the cartridge's own RAM (MBC3 takes $0A at $0000) and store one byte in it.
        constexpr std::string_view kWrites =
            "    ld a, $0A\n"
            "    ld [$0000], a\n"
            "    ld a, $5A\n"
            "    ld [$A000], a\n"
            "loop:\n"
            "    jr loop\n";
        Vm assembler{VMPlatform::GameBoyColor};
        Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "cartridge"}};
        VmTestAccess::saveFilesAt(machine, root);
        machine.hostRom(cartridgeRunning(assembler, kWrites, testing::kMbc3RamBattery, testing::kRam8K));
        const std::vector<std::uint8_t> before = machine.state();

        VmTestAccess::runInline(machine);
        for (int i = 0; i < 4; ++i) {
            VmTestAccess::stepOnce(machine);
        }
        machine.stop();
        EXPECT_EQ(firstByteOnDisk(), 0x5A);

        EXPECT_TRUE(machine.restore(before));
        VmTestAccess::flushSaveData(machine);
        EXPECT_NE(firstByteOnDisk(), 0x5A);
    }
    std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace retropp
