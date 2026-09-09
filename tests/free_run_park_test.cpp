// What a free-running machine waits for between steps: the arithmetic that says when its next step
// is due, and the park that honours it.
//
// A step is one whole frame of the machine's own time, landed at once, so the wall clock owes that
// frame for a while afterwards. The governor answers how long; the runner waits that long or its
// park interval, whichever comes first. The governor half is exact and asserted against clocks whose
// arithmetic reads by hand — one cycle per microsecond, so a thousand cycles is a millisecond. The
// runner half is asserted by what the two bounds do differently, never by how long a wait took.
#include "src/vm/run_governor.h"
#include "src/vm/vm_runner.h"

#include "retropp/driver_binding.h"
#include "retropp/vm.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>

#include <gtest/gtest.h>

namespace retropp {
namespace {

using namespace std::chrono_literals;

// One CGB frame's CPU-cycle budget — the per-step unit a runner drives.
const std::uint64_t kFrame = TimingProfile::GameBoyColor.cpuCyclesPerTick();

// A clock of one cycle per microsecond: a thousand cycles is a millisecond, so every expectation
// below is arithmetic the reader can do.
constexpr std::uint32_t kMicrosecondClock = 1'000'000;

// A tick that doubles whatever is in its mailbox into a slot, then clears the mailbox:
//   ld a,[0xC010] ; add a ; ld [0xC000],a ; xor a ; ld [0xC010],a ; ret
constexpr std::array<std::uint8_t, 12> kDoublingTick{0xFA, 0x10, 0xC0, 0x87, 0xEA, 0x00,
                                                     0xC0, 0xAF, 0xEA, 0x10, 0xC0, 0xC9};

DriverBinding doublingDriver() {
    DriverBinding b;
    b.images    = {DriverImage{.bytes = std::span<const std::uint8_t>(kDoublingTick), .base = 0x6000}};
    b.tickEntry = 0x6000;
    b.slots     = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read},
                   SlotSpec{.address = 0xC010, .width = 1, .direction = SlotDirection::Write}};
    return b;
}

// Wait for something a machine's own thread brings about. Generous against a loaded machine, and it
// returns what it saw rather than sleeping a fixed span, so a case fails on the condition it names.
template <typename Predicate>
bool waitUntil(Predicate done, std::chrono::milliseconds limit = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

// ── When the next step is due ─────────────────────────────────────────────────────────────────

// Cycles already owed are no wait at all — the machine steps rather than parks.
TEST(RunGovernor, CyclesAlreadyOwedAreNoWait) {
    vm::RunGovernor gov{kMicrosecondClock};
    const auto      t0 = std::chrono::steady_clock::now();
    gov.restart(t0);
    ASSERT_EQ(gov.owedThrough(t0 + 5ms), 5000u);  // 5 ms at a cycle per microsecond

    EXPECT_EQ(gov.timeUntilOwed(0), 0ns);
    EXPECT_EQ(gov.timeUntilOwed(5000), 0ns);  // exactly what is owed, and not a nanosecond more

    // The same boundary with a fraction of a cycle carried, where what is in hand already exceeds
    // what is still to come.
    ASSERT_EQ(gov.owedThrough(t0 + 5ms + 1500ns), 5001u);
    EXPECT_EQ(gov.timeUntilOwed(5001), 0ns);
}

// The wait is the owed arithmetic run backwards: the cycles still to come, at the clock's rate.
TEST(RunGovernor, TheWaitIsWhatTheOutstandingCyclesCost) {
    vm::RunGovernor gov{kMicrosecondClock};
    const auto      t0 = std::chrono::steady_clock::now();
    gov.restart(t0);

    EXPECT_EQ(gov.timeUntilOwed(1000), 1ms);   // a thousand cycles at one per microsecond
    EXPECT_EQ(gov.timeUntilOwed(16'000), 16ms);
}

// The factor scales the wait, exactly as it scales what accrues: twice the speed, half the wait.
TEST(RunGovernor, TheFactorScalesTheWait) {
    vm::RunGovernor gov{kMicrosecondClock};
    gov.restart(std::chrono::steady_clock::now());

    gov.setFactor(2, 1);
    EXPECT_EQ(gov.timeUntilOwed(1000), 500us);
    gov.setFactor(1, 2);
    EXPECT_EQ(gov.timeUntilOwed(1000), 2ms);
    gov.setFactor(1, 1);
    EXPECT_EQ(gov.timeUntilOwed(1000), 1ms);
}

// The carried remainder counts toward the wait, so a machine paced this way owes no fraction twice.
TEST(RunGovernor, TheCarriedRemainderShortensTheWait) {
    vm::RunGovernor gov{kMicrosecondClock};
    const auto      t0 = std::chrono::steady_clock::now();
    gov.restart(t0);
    // A cycle and a half accrues: one is owed, half is carried.
    ASSERT_EQ(gov.owedThrough(t0 + 1500ns), 1u);

    // Two more cycles are outstanding, and half of one is already in hand — so a cycle and a half
    // of wall time, not two.
    EXPECT_EQ(gov.timeUntilOwed(3), 1500ns);
}

// A wait ends owing the cycles asked for, never a nanosecond short of them.
TEST(RunGovernor, AWaitRoundsUpToTheCycleItIsWaitingFor) {
    vm::RunGovernor gov{7'000'000};  // a cycle every 142.857… nanoseconds
    gov.restart(std::chrono::steady_clock::now());

    EXPECT_EQ(gov.timeUntilOwed(1), 143ns);
}

// A pause never reaches the cycles asked for, and a target further out than one fold is not worth
// measuring — both report the fold, so a loop parking on this always looks up again.
TEST(RunGovernor, AWaitThatNeverEndsIsReportedAsTheFold) {
    vm::RunGovernor gov{kMicrosecondClock};
    gov.restart(std::chrono::steady_clock::now());

    EXPECT_EQ(gov.timeUntilOwed(1'000'000'000), vm::RunGovernor::kMaxFold);  // ~1000 s away
    gov.setFactor(0, 1);
    EXPECT_EQ(gov.timeUntilOwed(1), vm::RunGovernor::kMaxFold);  // paused, so never
}

// ── The park that honours it ──────────────────────────────────────────────────────────────────

// A runner waits for its deadline or its park interval, whichever comes first — so a deadline
// already met turns a park over promptly, where a deadline far out runs the interval.
TEST(VmRunner, AParkLastsTheDeadlineOrTheIntervalWhicheverIsShorter) {
    const auto parksIn = [](std::chrono::nanoseconds deadline, std::chrono::milliseconds window) {
        vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                            vm::VmRunner::Mode::Threaded};
        std::atomic<std::size_t> asked{0};
        runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
        // A backlog held at the mark: the machine has nothing to produce, so every pass parks.
        runner.start([] { return std::size_t{1}; }, 1, [&asked, deadline] {
            asked.fetch_add(1, std::memory_order_relaxed);
            return deadline;
        });
        std::this_thread::sleep_for(window);
        return asked.load(std::memory_order_relaxed);
    };

    const std::size_t onTheInterval = parksIn(1s, 60ms);   // far past the interval — it bounds the wait
    const std::size_t onTheDeadline = parksIn(0ns, 60ms);  // already due — the wait is the deadline

    EXPECT_GT(onTheInterval, 5u);  // asked afresh each time the interval ran out
    EXPECT_GT(onTheDeadline, onTheInterval * 2);
}

// The interval bounds a deadline reaching past it, so a machine with nothing due keeps looking up —
// and a runner asked to leave mid-park leaves without being woken.
TEST(VmRunner, TheParkIntervalBoundsADeadlineReachingPastIt) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                        vm::VmRunner::Mode::Threaded};
    std::atomic<std::size_t> asked{0};
    runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
    runner.start([] { return std::size_t{1}; }, 1, [&asked] {
        asked.fetch_add(1, std::memory_order_relaxed);
        return std::chrono::nanoseconds{1h};
    });

    // A third asking is only reached by parking twice over, so the interval — not the deadline — is
    // what ended those parks.
    ASSERT_TRUE(waitUntil([&] { return asked.load(std::memory_order_relaxed) >= 3; }, 2s));

    runner.requestStop();  // and no wake — the interval alone brings the loop back to look

    EXPECT_TRUE(waitUntil([&] { return runner.finished(); }, 2s));
}

// A runner given no deadline parks the interval and paces on its backlog alone.
TEST(VmRunner, ARunnerWithNoDeadlineStillParksAndResumes) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                        vm::VmRunner::Mode::Threaded};
    std::atomic<std::size_t> produced{0};
    std::atomic<std::size_t> taken{0};

    runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
    runner.afterEachStep([&] { produced.fetch_add(1, std::memory_order_relaxed); });
    runner.start(
        [&] {
            return produced.load(std::memory_order_relaxed) - taken.load(std::memory_order_relaxed);
        },
        2);
    ASSERT_TRUE(waitUntil([&] { return produced.load(std::memory_order_relaxed) >= 2; }));

    taken.fetch_add(1, std::memory_order_relaxed);  // the consumer took one

    EXPECT_TRUE(waitUntil([&] { return produced.load(std::memory_order_relaxed) >= 3; }));
}

}  // namespace
}  // namespace retropp
