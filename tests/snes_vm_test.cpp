// The generic host over the SNES core: a `Vm::SNES` constructs and runs on both clocks, `speed` scales
// it and `stop` parks it — everything above `VmBackend`, which the SNES gets by implementing the
// interface, none of it written for the SNES.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/vm/vm_testing.h"

namespace retropp {
namespace {

using vm::VmTestAccess;

// The SNES core's own budgets: one NTSC frame at the console's own period, the foreign-rate draw at a
// clean 60 Hz, and one PAL frame — the figures the core answers regardless of the profile a spelling
// names.
constexpr auto kNtscFrame   = std::chrono::nanoseconds{16'639'263};
constexpr auto kHz60Period  = std::chrono::nanoseconds{16'666'667};
constexpr auto kPalFrame    = std::chrono::nanoseconds{19'997'208};
constexpr std::uint64_t kNtscBudget  = 357'366u;
constexpr std::uint64_t kNtscForeign = 357'954u;
constexpr std::uint64_t kPalBudget   = 425'568u;

std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return rom;
}

constexpr std::string_view kIdle =
    "        ORG $00:8000\n"
    "here:   BRA here\n";

// A program that asks the chip to interlace and then rests: every frame after it is a field.
constexpr std::string_view kInterlaces =
    "        ORG $00:8000\n"
    "        SEP #$20\n"
    "        LDA #$01\n"
    "        STA !$2133\n"
    "here:   BRA here\n";

std::vector<std::uint8_t> ntscCartridge() { return cartridgeFrom(kIdle); }

// The same image with a PAL country byte, so the machine builds at the 50 Hz clock.
std::vector<std::uint8_t> palCartridge() {
    std::vector<std::uint8_t> rom = cartridgeFrom(kIdle);
    rom[0x7FC0u + 0x19u] = 0x02u;  // country byte: PAL
    return rom;
}

// Both budgets, framePeriod first so the carry the framePeriod path leaves at zero feeds the foreign one.
void expectOneSpeed(Vm& vm) {
    EXPECT_EQ(VmTestAccess::tickBudget(vm, kNtscFrame), kNtscBudget);
    EXPECT_EQ(VmTestAccess::tickBudget(vm, kHz60Period), kNtscForeign);
}

TEST(SnesVm, EverySpellingIsOneMachineOneSpeed) {
    // The machine's speed comes from its core (the NTSC SNES clock before it hosts anything), never from
    // the TimingProfile the spelling names — that is what the five spellings pin.
    { Vm vm{VMPlatform::Snes};                                    expectOneSpeed(vm); }
    { Vm vm{VMPlatform::Snes, TimingProfile::GameBoyColor};       expectOneSpeed(vm); }
    { Vm vm{VMPlatform::Snes, TimingProfile::Snes};              expectOneSpeed(vm); }
    { Vm vm{VMPlatform::Snes, TimingProfile{TickPeriodNs::Hz60}}; expectOneSpeed(vm); }
    { Vm::SNES vm;                                                expectOneSpeed(vm); }
}

TEST(SnesVm, AHostedCartridgeRunsOnTheTickClock) {
    Vm::SNES vm{VmConfig{.video = true}};
    vm.hostRom(ntscCartridge());
    vm.video(true);  // turn drawing on, so finished frames are counted
    vm.run(Vm::Advance::OnTick);

    const std::uint64_t g0 = vm.video().generation;
    vm.advanceTick();
    EXPECT_EQ(vm.video().generation, g0 + 1) << "one tick finishes one SNES frame";
    vm.advanceTick();
    EXPECT_EQ(vm.video().generation, g0 + 2);

    vm.stop();
}

// On a running machine the video settings land with the switch at the next step boundary, where every
// change issued to a running machine lands: asked between two ticks, the picture the game holds is the
// one the last tick latched, and the tick after the call answers with the fields woven.
TEST(SnesVm, VideoSettingsLandAtTheStepBoundaryOnARunningMachine) {
    Vm::SNES vm{VmConfig{.video = true}};
    vm.hostRom(cartridgeFrom(kInterlaces));
    vm.run(Vm::Advance::OnTick);
    for (int tick = 0; tick < 3; ++tick) {
        vm.advanceTick();
    }
    EXPECT_EQ(vm.video().height, 224);   // fields, shown as they come

    vm.video(true, {.interlacing = {.on = true}});
    EXPECT_EQ(vm.video().height, 224);   // asked, not landed: the last latched picture

    vm.advanceTick();                    // the change lands at the step's start; the step's field is woven
    EXPECT_EQ(vm.video().height, 448);
    EXPECT_EQ(vm.video().width, 256);
    vm.stop();
}

TEST(SnesVm, AHostedCartridgeRunsOnItsOwnThread) {
    Vm::SNES vm{VmConfig{.video = true}};
    vm.hostRom(ntscCartridge());
    vm.video(true);  // turn drawing on, so finished frames are counted
    vm.run(Vm::Advance::Continuously);

    const std::uint64_t start = vm.video().generation;
    const auto advanced = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (vm.video().generation > start) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return vm.video().generation > start;
    };
    EXPECT_TRUE(advanced()) << "the machine never drew a frame on its own thread";

    vm.stop();
}

TEST(SnesVm, SpeedScalesTheMachine) {
    { Vm::SNES vm; EXPECT_EQ(VmTestAccess::tickBudget(vm, kNtscFrame), kNtscBudget); }  // default 1:1

    {
        Vm::SNES vm;
        vm.speed(1, 2);
        EXPECT_EQ(vm.speed().first, 1u);
        EXPECT_EQ(vm.speed().second, 2u);
        EXPECT_LT(VmTestAccess::tickBudget(vm, kNtscFrame), kNtscBudget) << "half speed buys fewer cycles";
    }
    {
        Vm::SNES vm;
        vm.speed(2, 1);
        EXPECT_EQ(vm.speed().first, 2u);
        EXPECT_EQ(vm.speed().second, 1u);
        EXPECT_GT(VmTestAccess::tickBudget(vm, kNtscFrame), kNtscBudget) << "double speed buys more";
    }
    {
        Vm::SNES vm;
        vm.speed(0, 1);
        EXPECT_EQ(vm.speed().first, 0u);
        EXPECT_EQ(VmTestAccess::tickBudget(vm, kNtscFrame), 0u) << "paused buys nothing";
    }
}

TEST(SnesVm, HostingAPalCartridgeRebuildsTheGovernorAndKeepsTheFactor) {
    // The clock follows the cartridge: hosting a 50 Hz image answers the PAL budget, whatever spelling
    // built the machine.
    { Vm vm{VMPlatform::Snes};                          vm.hostRom(palCartridge()); EXPECT_EQ(VmTestAccess::tickBudget(vm, kPalFrame), kPalBudget); }
    { Vm vm{VMPlatform::Snes, TimingProfile::Snes};     vm.hostRom(palCartridge()); EXPECT_EQ(VmTestAccess::tickBudget(vm, kPalFrame), kPalBudget); }
    { Vm::SNES vm;                                       vm.hostRom(palCartridge()); EXPECT_EQ(VmTestAccess::tickBudget(vm, kPalFrame), kPalBudget); }

    // A factor set before hosting survives the governor rebuild the new clock triggers.
    Vm::SNES vm;
    vm.speed(1, 2);
    vm.hostRom(palCartridge());
    EXPECT_EQ(vm.speed().first, 1u);
    EXPECT_EQ(vm.speed().second, 2u);
}

}  // namespace
}  // namespace retropp
