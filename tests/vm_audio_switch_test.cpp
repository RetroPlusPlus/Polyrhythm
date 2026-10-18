// The audio switch, on the host layer.
//
// What is pinned here is the HOST layer's switch — the settings a machine holds, the step boundary a
// change to a running machine lands at, the refusals at the call — on a core whose sound is incidental
// to the cases: a Game Boy hosting a cartridge of zeros, whose sound chip delivers one frame per sample
// period whatever the program does. One Game Boy frame at 48'000 is 803 or 804 of them; at 24'000, 401
// or 402. Every count is a DELTA between two consecutive ticks, never a count from zero — the first tick
// after run carries the boot and is not measured. The mock serves one case: the refusal.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::AudioOptions;
using retropp::AudioSampleFn;
using retropp::Vm;
using retropp::VmConfig;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
using retropp::vm::VmTestAccess;

// The frames one Game Boy frame delivers at each rate, with the room the cadence's rounding needs.
constexpr double kFramesAt48k = 803.0;
constexpr double kFramesAt24k = 402.0;
constexpr double kRoomAt48k   = 8.0;
constexpr double kRoomAt24k   = 4.0;

// A Vm driven by the mock — the core that makes no sound.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoy};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

std::vector<std::uint8_t> zeroRom() { return std::vector<std::uint8_t>(0x8000, 0x00); }

// A function that counts the frames it is given. Atomic, because the continuously advancing machine
// calls it from its own thread.
struct Counting {
    std::atomic<std::uint64_t> count{0};

    AudioSampleFn fn() {
        return [this](std::int16_t, std::int16_t) { count.fetch_add(1, std::memory_order_relaxed); };
    }
    [[nodiscard]] std::uint64_t now() const { return count.load(std::memory_order_relaxed); }
};

// One tick, and how many frames it delivered to `c`.
std::uint64_t tickDelta(Vm& machine, const Counting& c) {
    const std::uint64_t before = c.now();
    machine.advanceTick();
    return c.now() - before;
}

// ── The switch and its held settings ────────────────────────────────────────────────────────────

TEST(VmAudioSwitch, AFunctionGivenWithTheSwitchOffReceivesNothing) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.audio(false, AudioOptions{.onSample = c.fn()});
    machine.hostRom(zeroRom());
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    EXPECT_EQ(tickDelta(machine, c), 0u);
    EXPECT_EQ(tickDelta(machine, c), 0u);
}

TEST(VmAudioSwitch, OptionsGivenWithTheSwitchOffAreHeldForWhenItIsNextOn) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.audio(false, AudioOptions{.rate = 24'000, .onSample = c.fn()});
    machine.audio(true);
    machine.hostRom(zeroRom());
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt24k, kRoomAt24k);
}

TEST(VmAudioSwitch, AudioDeclaredAtConstructionIsOnAtTheHeldRate) {
    Vm       machine{VMPlatform::GameBoy, VmConfig{.audio = true}};
    Counting c;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = c.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);
}

TEST(VmAudioSwitch, ASoundlessCoreRefusesTheSwitchOnAndAcceptsItOff) {
    MockedVm m;
    EXPECT_THROW(m.machine.audio(true), std::logic_error);
    EXPECT_NO_THROW(m.machine.audio(false));
    EXPECT_NO_THROW(m.machine.audio(false, AudioOptions{.rate = 24'000}));
}

TEST(VmAudioSwitch, AskingForAudioTwiceIsIdempotent) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = c.fn()});
    machine.audio(true);
    machine.audio(true);
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    // Once per frame — not twice, not none.
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);

    // And however many times it was asked on, one off is off: the switch is a level, not a count. Each
    // ask lands at its own boundary, so the two are not merged into one.
    machine.audio(true);
    machine.advanceTick();
    machine.audio(false);
    EXPECT_EQ(tickDelta(machine, c), 0u);
}

TEST(VmAudioSwitch, AnUnsetRateKeepsTheHeldOne) {
    Vm       machine{VMPlatform::GameBoy};
    Counting a;
    Counting b;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.rate = 24'000, .onSample = a.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, a)), kFramesAt24k, kRoomAt24k);

    machine.audio(true, AudioOptions{.onSample = b.fn()});
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, b)), kFramesAt24k, kRoomAt24k);  // 402, not 803
}

TEST(VmAudioSwitch, AnUnsetFunctionKeepsTheHeldOne) {
    Vm       machine{VMPlatform::GameBoy};
    Counting a;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.rate = 48'000, .onSample = a.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, a)), kFramesAt48k, kRoomAt48k);

    machine.audio(true, AudioOptions{.rate = 24'000});
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, a)), kFramesAt24k, kRoomAt24k);
}

TEST(VmAudioSwitch, OffStopsTheFramesAndOnResumesThem) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = c.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);

    machine.audio(false);
    EXPECT_EQ(tickDelta(machine, c), 0u);

    machine.audio(true);
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);
}

// ── The step boundary ───────────────────────────────────────────────────────────────────────────

TEST(VmAudioSwitch, ASwitchOnARunningMachineLandsBeforeItsNextStep) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.hostRom(zeroRom());
    machine.run(Vm::Advance::OnTick);

    machine.audio(true, AudioOptions{.onSample = c.fn()});
    EXPECT_EQ(c.now(), 0u);  // queued, not applied

    // The change landed at the boundary that precedes the step, not after it.
    machine.advanceTick();
    EXPECT_GT(c.now(), 0u);
}

TEST(VmAudioSwitch, AMachineOnItsOwnClockSounds) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.hostRom(zeroRom());
    machine.run(Vm::Advance::Continuously);

    EXPECT_NO_THROW(machine.audio(true, AudioOptions{.onSample = c.fn()}));

    // Liveness, not a count: the machine steps at its own cadence, so only time bounds this.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (c.now() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_GT(c.now(), 0u) << "the machine never sounded on its own clock";
    machine.stop();
}

// A stop() that left a queued change behind would land it at the next run's first boundary, over
// whatever the parked machine was told in between.
TEST(VmAudioSwitch, AChangeIssuedBeforeStopStillLands) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = c.fn()});
    machine.run(Vm::Advance::Continuously);
    machine.audio(false);  // queued for the running machine
    machine.stop();        // drained here: off
    machine.audio(true);   // parked: direct
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);
}

TEST(VmAudioSwitch, TwoChangesBeforeOneBoundaryMerge) {
    Vm       machine{VMPlatform::GameBoy};
    Counting a;
    Counting b;
    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = a.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    // A member the later call sets wins; one it leaves unset keeps the earlier call's.
    machine.audio(true, AudioOptions{.rate = 24'000});
    machine.audio(true, AudioOptions{.onSample = b.fn()});
    machine.advanceTick();
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, b)), kFramesAt24k, kRoomAt24k);
}

// ── Refused at the call ─────────────────────────────────────────────────────────────────────────

TEST(VmAudioSwitch, AZeroRateIsRefusedAtTheCallAndChangesNothing) {
    Vm       machine{VMPlatform::GameBoy};
    Counting c;
    EXPECT_THROW(machine.audio(true, AudioOptions{.rate = 0}), std::invalid_argument);
    EXPECT_THROW(machine.audio(false, AudioOptions{.rate = 0}), std::invalid_argument);

    machine.hostRom(zeroRom());
    machine.audio(true, AudioOptions{.onSample = c.fn()});
    machine.run(Vm::Advance::OnTick);
    machine.advanceTick();

    // On a running machine too: refused at the call, nothing queued, nothing changed.
    EXPECT_THROW(machine.audio(true, AudioOptions{.rate = 0}), std::invalid_argument);
    EXPECT_NEAR(static_cast<double>(tickDelta(machine, c)), kFramesAt48k, kRoomAt48k);
}

}  // namespace
