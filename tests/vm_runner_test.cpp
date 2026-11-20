// The runner's own semantics, device-free: what a step does to the machine it owns, when a queued
// gesture reaches that machine, and what the after-step hook sees.
//
// Each case builds a runner over a machine configured from hand-assembled SM83 images (NO ROM —
// surgically-placed `const` byte blobs) and observes the result through a declared slot, so the
// assertions are about the runner's scheduling rather than any driver's sound. Every VM core is
// deterministic, so a placed routine's effect on RAM is reproducible without a device.
#include "src/vm/vm_runner.h"

#include "src/vm/machine_threads.h"

#include "retropp/driver_binding.h"
#include "retropp/vm.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

// One CGB frame's CPU-cycle budget — the per-step unit a voice's runner drives.
const std::uint64_t kFrame = TimingProfile::GameBoyColor.cpuCyclesPerTick();

// A tick that doubles whatever is in its mailbox into a slot, then clears the mailbox:
//   ld a,[0xC010] ; add a ; ld [0xC000],a ; xor a ; ld [0xC010],a ; ret
// Clearing is what makes a re-performed gesture visible — a second step over an untouched mailbox
// would report the same doubled value again.
constexpr std::array<std::uint8_t, 12> kDoublingTick{0xFA, 0x10, 0xC0, 0x87, 0xEA, 0x00,
                                                     0xC0, 0xAF, 0xEA, 0x10, 0xC0, 0xC9};

// The binding for that tick: slot 0 is the result the game reads, slot 1 the mailbox it writes.
DriverBinding doublingDriver() {
    DriverBinding b;
    b.images    = {DriverImage{.bytes = std::span<const std::uint8_t>(kDoublingTick), .base = 0x6000}};
    b.tickEntry = 0x6000;
    b.slots     = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read},
                   SlotSpec{.address = 0xC010, .width = 1, .direction = SlotDirection::Write}};
    return b;
}

// A gesture that delivers `value` to that driver's mailbox.
Instruction mailboxWrite(std::uint64_t value) {
    return Instruction::write(Location::memory(0xC010), 1, value);
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
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return done();
}

// A resident runner over the doubling driver, ready to step.
std::unique_ptr<vm::VmRunner> residentRunner() {
    auto runner = std::make_unique<vm::VmRunner>(Vm{VMPlatform::GameBoyColor},
                                                 vm::VmRunner::StepKind::Resident, kFrame);
    runner->machine().hostDriver(doublingDriver());
    return runner;
}

// ── The step ──────────────────────────────────────────────────────────────────────────────────

// A started driver's step advances its machine by the runner's budget. The routine is placed through
// machine() and keeps running across steps, which is what the runner's fixed machine address buys.
TEST(VmRunner, StartedStepAdvancesTheMachineByItsBudget) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Started, kFrame};
    static constexpr std::array<std::uint8_t, 2> kSpin{0x18, 0xFE};  // jr $ — runs until the budget ends
    const Routine<void()> driver = runner.machine().uploadRoutine<void()>(
        std::span<const std::uint8_t>(kSpin), RoutineBinding{.throttle = Throttle::HardwareSpeed});
    runner.machine().startDriver(driver);

    const std::uint64_t first  = runner.stepOnce();
    const std::uint64_t second = runner.stepOnce();

    EXPECT_GE(first, kFrame);       // a step runs the whole budget
    EXPECT_LT(first, kFrame * 2);   // and stops there (a partial last instruction overshoots slightly)
    EXPECT_GE(second, kFrame);      // the driver is still running — each step advances it again
    EXPECT_LT(second, kFrame * 2);
}

// A resident driver's step performs the queued gestures first, then calls the tick — so the tick acts
// on what the gesture delivered, within the one step.
TEST(VmRunner, ResidentStepPerformsQueuedGesturesThenTicks) {
    const std::unique_ptr<vm::VmRunner> runner = residentRunner();
    runner->enqueue(mailboxWrite(5));

    runner->stepOnce();

    EXPECT_EQ(runner->machine().readSlot(0), 10u);  // the tick doubled the value the gesture delivered
}

// Gestures are performed in submission order, so the last write to a mailbox is the one the tick reads.
TEST(VmRunner, GesturesArePerformedInSubmissionOrder) {
    const std::unique_ptr<vm::VmRunner> runner = residentRunner();
    runner->enqueue(mailboxWrite(3));
    runner->enqueue(mailboxWrite(5));

    runner->stepOnce();

    EXPECT_EQ(runner->machine().readSlot(0), 10u);  // 5 arrived last — 3 would have doubled to 6
}

// A gesture is performed once. The next step finds an empty mailbox and the tick acts on the cleared
// value.
TEST(VmRunner, AGestureIsPerformedOnceAndNotRedelivered) {
    const std::unique_ptr<vm::VmRunner> runner = residentRunner();
    runner->enqueue(mailboxWrite(5));

    runner->stepOnce();
    EXPECT_EQ(runner->machine().readSlot(0), 10u);

    runner->stepOnce();
    EXPECT_EQ(runner->machine().readSlot(0), 0u);  // the mailbox the tick cleared stayed cleared
}

// A gesture offered after a step waits for the next one — the step boundary is where a machine takes
// delivery. The opening gesture settles the mailbox: a machine powers on with work RAM already
// holding values, so the starting point is established rather than assumed.
TEST(VmRunner, AGestureOfferedAfterAStepWaitsForTheNext) {
    const std::unique_ptr<vm::VmRunner> runner = residentRunner();
    runner->enqueue(mailboxWrite(0));
    runner->stepOnce();
    ASSERT_EQ(runner->machine().readSlot(0), 0u);

    runner->enqueue(mailboxWrite(7));
    EXPECT_EQ(runner->machine().readSlot(0), 0u);  // the gesture has not reached the machine yet

    runner->stepOnce();
    EXPECT_EQ(runner->machine().readSlot(0), 14u);
}

// ── The hook ──────────────────────────────────────────────────────────────────────────────────

// The hook runs after each step, and sees what that step produced.
TEST(VmRunner, TheHookRunsAfterEachStepAndSeesItsResult) {
    const std::unique_ptr<vm::VmRunner> runner = residentRunner();
    int                       ran = 0;
    std::vector<std::uint64_t> seen;
    runner->afterEachStep([&] {
        ++ran;
        seen.push_back(runner->machine().readSlot(0));
    });

    runner->enqueue(mailboxWrite(5));
    runner->stepOnce();
    runner->stepOnce();

    EXPECT_EQ(ran, 2);                                     // once per step
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], 10u);                               // after the tick, not before it
    EXPECT_EQ(seen[1], 0u);
}

// ── Gesture eligibility ───────────────────────────────────────────────────────────────────────

// A gesture is performed through a resident machine's declared entries. A started driver has none, so
// offering one is a usage error rather than a value that quietly goes nowhere.
TEST(VmRunner, EnqueueOnAStartedRunnerThrows) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Started, kFrame};
    EXPECT_THROW(runner.enqueue(mailboxWrite(1)), std::logic_error);
}

// ── Placement ─────────────────────────────────────────────────────────────────────────────────

// An inline runner is stepped by whoever calls it, so its content is placed by that same caller, then
// and there — which is what keeps an error in the content an error at the call that supplied it.
TEST(VmRunner, AnInlineRunnerPlacesItsContentWhereItIsDeclared) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame};
    bool placed = false;

    runner.beforeFirstStep([&] {
        placed = true;
        runner.machine().hostDriver(doublingDriver());
    });

    EXPECT_TRUE(placed);  // no step was needed
}

// A threaded runner's content is placed by the machine's own thread, before that machine's first step.
TEST(VmRunner, AThreadedRunnerPlacesItsContentOnItsOwnThreadBeforeStepping) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                        vm::VmRunner::Mode::Threaded};
    std::atomic<bool> placed{false};
    std::atomic<int>  steps{0};
    std::thread::id   placedOn;

    runner.beforeFirstStep([&] {
        placedOn = std::this_thread::get_id();
        runner.machine().hostDriver(doublingDriver());
        placed.store(true, std::memory_order_release);
    });
    runner.afterEachStep([&] { steps.fetch_add(1, std::memory_order_relaxed); });
    // A backlog already at the mark: the machine places its content and then has nothing to produce,
    // so this observes placement on its own.
    runner.start([] { return std::size_t{1}; }, 1);

    ASSERT_TRUE(waitUntil([&] { return placed.load(std::memory_order_acquire); }));
    EXPECT_NE(placedOn, std::this_thread::get_id());
    EXPECT_EQ(steps.load(std::memory_order_relaxed), 0);  // placement came first
}

// ── Pacing ────────────────────────────────────────────────────────────────────────────────────

// A threaded machine runs ahead of its consumer by the high-water mark and stops there, so what it
// produces is bounded by what has been taken from it.
TEST(VmRunner, AThreadedRunnerRunsAheadToItsHighWaterMarkAndStops) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                        vm::VmRunner::Mode::Threaded};
    std::atomic<std::size_t> produced{0};

    runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
    runner.afterEachStep([&] { produced.fetch_add(1, std::memory_order_relaxed); });
    runner.start([&] { return produced.load(std::memory_order_relaxed); }, 3);

    ASSERT_TRUE(waitUntil([&] { return produced.load(std::memory_order_relaxed) >= 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    EXPECT_EQ(produced.load(std::memory_order_relaxed), 3u);  // and no further
}

// Taking what a parked machine produced lets it produce again.
TEST(VmRunner, AParkedRunnerResumesWhenItsBacklogDrains) {
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
        3);
    ASSERT_TRUE(waitUntil([&] { return produced.load(std::memory_order_relaxed) >= 3; }));

    taken.fetch_add(1, std::memory_order_relaxed);  // the consumer took one
    runner.wake();

    ASSERT_TRUE(waitUntil([&] { return produced.load(std::memory_order_relaxed) >= 4; }));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    EXPECT_EQ(produced.load(std::memory_order_relaxed), 4u);  // back at the mark, and parked again
}

// ── Leaving ───────────────────────────────────────────────────────────────────────────────────

// A threaded runner leaves when asked, and says so — which is what lets a closing voice be set aside
// and destroyed later instead of waited on.
TEST(VmRunner, AThreadedRunnerLeavesWhenAskedAndSaysSo) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                        vm::VmRunner::Mode::Threaded};
    runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
    runner.start([] { return std::size_t{0}; }, 1);  // never full — it steps continuously
    EXPECT_FALSE(runner.finished());

    runner.requestStop();
    runner.wake();

    EXPECT_TRUE(waitUntil([&] { return runner.finished(); }));
}

// An inline runner is never inside a loop, so it has always already left — a voice stepped by hand
// closes in the pass that decides to close it.
TEST(VmRunner, AnInlineRunnerHasAlwaysFinished) {
    vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame};
    EXPECT_TRUE(runner.finished());
}

// ── The machine count ─────────────────────────────────────────────────────────────────────────

// A threaded runner counts among the machines on their own thread from its start until it is gone, which
// is what a worker pool made in that span leaves a core for. An inline runner is never counted.
TEST(VmRunner, AThreadedRunnerIsCountedWhileItsThreadRuns) {
    const std::size_t before = vm::machineThreads.load();
    {
        vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame,
                            vm::VmRunner::Mode::Threaded};
        runner.beforeFirstStep([&] { runner.machine().hostDriver(doublingDriver()); });
        EXPECT_EQ(vm::machineThreads.load(), before);
        runner.start([] { return std::size_t{1}; }, 1);
        EXPECT_EQ(vm::machineThreads.load(), before + 1);
    }
    EXPECT_EQ(vm::machineThreads.load(), before);

    {
        vm::VmRunner runner{Vm{VMPlatform::GameBoyColor}, vm::VmRunner::StepKind::Resident, kFrame};
        runner.machine().hostDriver(doublingDriver());
        runner.stepOnce();
        EXPECT_EQ(vm::machineThreads.load(), before);
    }
    EXPECT_EQ(vm::machineThreads.load(), before);
}

}  // namespace
}  // namespace retropp
