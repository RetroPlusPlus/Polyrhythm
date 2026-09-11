// Reaching a hosted machine's buttons: what the game hands over, when it lands, and what the guest
// reads when it looks.
//
// Three layers, each asserted where it lives. The packing is pure arithmetic on gb::Buttons, so it is
// static_assert-testable. The delivery is the Vm's, observed through a mock backend that records every
// word that crossed the seam and in which step. What a guest actually SEES is asserted against a real
// SM83 core running an authored cartridge that polls the joypad and stores what it found — the only
// layer that proves the register-level half.
#include "retropp/gb.h"
#include "retropp/guest_buttons.h"
#include "retropp/vm.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/vm/vm_testing.h"
#include "tests/authored_cartridge.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::GuestButtons;
using retropp::MemoryRegion;
using retropp::RegionMapId;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
using retropp::vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for reading what crossed the seam.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoy};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

// ── The packing: what a button state IS ─────────────────────────────────────────────────────────

// The word a button state converts to.
constexpr std::uint64_t bits(retropp::gb::Buttons b) noexcept { return GuestButtons(b).held; }

// The joypad's own order — right, left, up, down, a, b, select, start from bit 0.
TEST(GbButtons, EachButtonIsItsOwnBit) {
    static_assert(bits(retropp::gb::Buttons{}) == 0, "nothing held is no bits");
    static_assert(bits(retropp::gb::Buttons{.right = true}) == 1u << 0);
    static_assert(bits(retropp::gb::Buttons{.left = true}) == 1u << 1);
    static_assert(bits(retropp::gb::Buttons{.up = true}) == 1u << 2);
    static_assert(bits(retropp::gb::Buttons{.down = true}) == 1u << 3);
    static_assert(bits(retropp::gb::Buttons{.a = true}) == 1u << 4);
    static_assert(bits(retropp::gb::Buttons{.b = true}) == 1u << 5);
    static_assert(bits(retropp::gb::Buttons{.select = true}) == 1u << 6);
    static_assert(bits(retropp::gb::Buttons{.start = true}) == 1u << 7);

    // Several at once, and a field left out is released rather than unset.
    static_assert(bits(retropp::gb::Buttons{.left = true, .a = true}) == ((1u << 1) | (1u << 4)));
    EXPECT_EQ(bits(retropp::gb::Buttons{.left = true, .a = true}), (1u << 1) | (1u << 4));
}

// ── Reading the vocabulary back ─────────────────────────────────────────────────────────────────

// Hold these Button actions this tick, the way the run loop hands a sampled level to the game.
void hold(retropp::InputState& in, std::initializer_list<retropp::gb::Button> down) {
    retropp::ActionSet level;
    for (const retropp::gb::Button b : down) {
        level.set(retropp::actionId(b), true);
    }
    std::array<retropp::ActionSet, retropp::kMaxPlayers> pressed{};
    pressed[0] = level;
    retropp::InputSample sample;
    sample.players[0].held = level;
    in.sampleTick(sample, pressed);
}

// held() reads the eight Button actions and nothing else — the map that bound them is the game's.
TEST(GbHeld, ReadsTheButtonActionsHeldThisTick) {
    retropp::InputState in;
    hold(in, {retropp::gb::Button::A, retropp::gb::Button::Left});

    const retropp::gb::Buttons pad = retropp::gb::held(in);
    EXPECT_TRUE(pad.a);
    EXPECT_TRUE(pad.left);
    EXPECT_FALSE(pad.b);
    EXPECT_FALSE(pad.right);
    EXPECT_EQ(GuestButtons(pad).held, (1u << 1) | (1u << 4));

    hold(in, {});
    EXPECT_EQ(GuestButtons(retropp::gb::held(in)).held, 0u) << "released is simply not held";
}

// A tap pressed and released between two ticks has no level left to read at tick time. The run loop
// accumulates it anyway, and the guest samples its joypad on its own frame — so the tap has to reach
// the machine as one frame of held, or a quick press is silently lost.
TEST(GbHeld, ATapShorterThanATickStillReaches) {
    retropp::InputState in;

    // The level at tick time is nothing; the union since the last tick carries the press.
    std::array<retropp::ActionSet, retropp::kMaxPlayers> pressed{};
    pressed[0].set(retropp::actionId(retropp::gb::Button::A), true);
    retropp::InputSample sample;  // held stays empty — the button is already back up
    in.sampleTick(sample, pressed);

    EXPECT_TRUE(retropp::gb::held(in).a) << "the tap was dropped — a press the player made is gone";

    // And it lasts exactly the one tick, so a tap is a tap rather than a stuck button.
    in.sampleTick(retropp::InputSample{}, std::array<retropp::ActionSet, retropp::kMaxPlayers>{});
    EXPECT_FALSE(retropp::gb::held(in).a);
}

// ── Delivery: when the state reaches the machine ────────────────────────────────────────────────

// A machine with no thread of its own takes the state where it was asked for — there is no boundary
// to wait for.
TEST(GuestInput, AParkedMachineTakesTheStateAtOnce) {
    MockedVm m;
    m.machine.buttons(retropp::gb::Buttons{.a = true, .start = true});

    ASSERT_EQ(m.mock->buttonsSet.size(), 1u);
    EXPECT_EQ(m.mock->buttonsSet.front(), (1u << 4) | (1u << 7));
}

// The whole set is replaced every time, so releasing is submitting a value without that button in it.
TEST(GuestInput, ReleasingIsSubmittingTheSetWithoutIt) {
    MockedVm m;
    m.machine.buttons(retropp::gb::Buttons{.a = true});
    m.machine.buttons(retropp::gb::Buttons{});

    ASSERT_EQ(m.mock->buttonsSet.size(), 2u);
    EXPECT_EQ(m.mock->buttonsSet[0], 1u << 4);
    EXPECT_EQ(m.mock->buttonsSet[1], 0u);
}

// A core with no input path refuses on the calling thread, rather than failing later on the machine's.
TEST(GuestInput, ACoreThatTakesNoButtonsRefuses) {
    MockedVm m;
    m.mock->takesButtons_ = false;

    EXPECT_THROW(m.machine.buttons(retropp::gb::Buttons{.a = true}), std::logic_error);
    EXPECT_TRUE(m.mock->buttonsSet.empty());
}

// ── What the guest reads ────────────────────────────────────────────────────────────────────────

struct Places {
    MemoryRegion hram;
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(retropp::regions(
        retropp::region(&Places::hram, MemoryRegion{.at = 0xFF80, .size = 1, .count = 8}, "hram")));
}

std::uint8_t hram(Vm& machine, const RegionMapId<Places>& places, std::uint32_t index) {
    return machine.read(places, &Places::hram, index).at(0);
}

// A cartridge whose entry jumps to `mainSource` at $0150, with a ret past the CGB overlay window.
std::vector<std::uint8_t> runnableCartridge(Vm& assembler, std::string_view mainSource) {
    std::vector<std::uint8_t> rom = retropp::testing::authorCartridge(
        retropp::testing::kSmallestCartridge);
    rom[0x143] = 0x80;
    rom[0x040] = 0xD9;                                       // VBlank vector: reti
    rom[0x100] = 0x00;
    rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;  // jp $0150
    rom[0x900] = 0xC9;
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(mainSource));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

// Polls the joypad forever, storing what it last saw. $FF80 holds the action buttons (a, b, select,
// start) and $FF81 the directions, each inverted so a held button reads as a 1 bit — the register
// itself reads a held line LOW, and inverting at the source keeps the assertions readable.
constexpr std::string_view kJoypadPollSource = R"(
loop:
    ld a, $10
    ldh [$00], a        ; P1: select the action buttons
    ldh a, [$00]
    ldh a, [$00]        ; read twice — the lines settle
    cpl
    and $0F
    ldh [$80], a
    ld a, $20
    ldh [$00], a        ; P1: select the directions
    ldh a, [$00]
    ldh a, [$00]
    cpl
    and $0F
    ldh [$81], a
    jr loop
)";

// The headline: a button the game holds is a button the guest's own code reads.
TEST(GuestInput, AHeldButtonIsWhatTheGuestReads) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kJoypadPollSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);

    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x00) << "nothing is held, so the guest reads nothing";

    machine.buttons(retropp::gb::Buttons{.a = true, .start = true});
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x09) << "A is bit 0 of the action nibble, Start bit 3";

    machine.buttons(retropp::gb::Buttons{.left = true});
    VmTestAccess::stepOnce(machine);
    EXPECT_EQ(hram(machine, places, 0), 0x00) << "the action buttons were released with the set";
    EXPECT_EQ(hram(machine, places, 1), 0x02) << "Left is bit 1 of the direction nibble";

    machine.stop();
}

// A machine running on a thread of its own takes buttons submitted from the game's, and the guest
// reads them. The hand-off is the reason the state waits for a step boundary rather than reaching the
// core where it was submitted — this case is the one that exercises it across two threads, and it is
// the case ThreadSanitizer reads to confirm nothing races.
TEST(GuestInput, AFreeRunningMachineTakesButtonsFromTheGamesThread) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kJoypadPollSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    machine.run(Vm::Advance::Continuously);

    machine.buttons(retropp::gb::Buttons{.b = true});

    const auto sawB = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (hram(machine, places, 0) == 0x02) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return hram(machine, places, 0) == 0x02;
    };
    EXPECT_TRUE(sawB()) << "B never reached the guest on its own thread";

    machine.stop();
}

// Two submissions between boundaries are a level, not a queue — the machine sees the later one and
// the earlier one never reaches it.
TEST(GuestInput, TheLastSubmissionBeforeABoundaryIsTheOneThatLands) {
    Vm::GBC machine;
    machine.hostRom(runnableCartridge(machine, kJoypadPollSource));
    const RegionMapId<Places> places = declarePlaces(machine);
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);

    machine.buttons(retropp::gb::Buttons{.a = true});
    machine.buttons(retropp::gb::Buttons{.select = true});
    VmTestAccess::stepOnce(machine);

    EXPECT_EQ(hram(machine, places, 0), 0x04) << "Select alone — the A submission was replaced, not queued";
    machine.stop();
}

}  // namespace
