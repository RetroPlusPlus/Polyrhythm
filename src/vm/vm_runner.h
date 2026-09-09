#ifndef RETROPP_SRC_VM_VM_RUNNER_H
#define RETROPP_SRC_VM_VM_RUNNER_H

// The one home of continuous-execution scheduling: a machine, the gestures waiting for it, and the step
// that advances it. Every continuously-running machine in the engine is driven through a runner.
//
// A runner drives a machine it owns or a machine it borrows. The owning form takes the Vm by value:
// everything placed into that machine — a driver routine, a hosted image, the APU sink — is reached
// through machine(), so a handle into it stays valid for as long as the runner lives; a runner is
// neither copyable nor movable, which is what keeps the machine's address fixed under a Routine's
// non-owning pointer. The borrowing form takes a pointer to a machine whose public owner is elsewhere
// (a Vm running its own hosted cartridge drives itself through a borrowed runner); the owner keeps the
// machine alive and at its address for as long as the runner lives. A ROM-hosting machine can hold no
// Routine handles, so the address-stability duty the owning form exists for never lands on a borrower.
//
// Gestures arrive through enqueue() and are performed at the next step boundary, in submission order,
// inside that step's cycle budget. The mailbox is the engine's SPSC hand-off — one thread enqueues, the
// stepping thread drains — so a gesture never reaches the machine mid-instruction.
//
// A step is one machine frame, and what that means depends on how the machine runs:
//   Started  — it runs continuously from a placed entry, so a step advances it by the budget.
//   Resident — it performs the drained gestures, calls its tick entry, then idles the remainder of the
//              budget so its hardware synthesizes at the original cadence.
// After the step, the installed hook runs. The hook is how a consumer reads what the step produced
// without the runner knowing what is being read.
//
// A machine either steps on the thread that asks it to (Inline) or on a thread of its own (Threaded).
// A threaded machine paces itself against what it has already produced: it steps while its downstream
// backlog is below the high-water mark and parks otherwise, so it runs ahead by a bounded amount and a
// machine that falls behind falls behind alone. A runner given a deadline parks until its next step is
// due, so the step lands on its own schedule rather than on the interval a park would otherwise run.
//
// A runner drives a machine, whatever that machine is for.
//
// INTERNAL — under src/vm/, never include/retropp/.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "retropp/driver_binding.h"  // Instruction — the gesture a mailbox carries
#include "retropp/vm.h"              // Vm — the machine a runner owns
#include "src/audio/ring_buffer.h"   // SpscRingBuffer — the engine's lock-free cross-thread hand-off

namespace retropp::vm {

class VmRunner {
public:
    // How a step advances the machine.
    enum class StepKind { Started, Resident };

    // Which thread steps the machine: the one that calls stepOnce(), or the runner's own.
    enum class Mode { Inline, Threaded };

    // Gestures the mailbox holds between steps. It matches the depth of the cue channel a gesture
    // reaches the mailbox through, so the upstream bound is the binding one; a full mailbox refuses the
    // gesture rather than blocking the thread that offered it.
    static constexpr std::size_t kMailboxCapacity = 256;

    // The longest a threaded runner parks once it has produced its fill. Short against the backlog
    // the high-water mark holds, so the machine resumes well before its consumer runs out; the wait
    // is timed rather than pure, so a missed wake costs one interval and never a stall. A runner
    // given a deadline waits for whichever comes first, and this is what keeps that wait finite —
    // a paused machine still looks up often enough to notice it has been asked to leave.
    static constexpr std::chrono::milliseconds kParkInterval{4};

    // Take ownership of `machine` and drive it `cyclesPerStep` CPU cycles at a time. Pass a machine that
    // is otherwise untouched: place its content through beforeFirstStep(), so every handle into it is
    // taken on the thread that will step it, at the address it keeps.
    VmRunner(Vm machine, StepKind kind, std::uint64_t cyclesPerStep, Mode mode = Mode::Inline);

    // Drive a machine the caller owns. The machine outlives the runner and stays at its address while
    // the runner lives; content already placed in it stays placed — the runner adds scheduling, not
    // ownership.
    VmRunner(Vm* machine, StepKind kind, std::uint64_t cyclesPerStep, Mode mode = Mode::Inline);

    // Leaving the loop and waiting for it, in that order — a machine mid-step finishes that step.
    ~VmRunner();

    VmRunner(const VmRunner&) = delete;
    VmRunner& operator=(const VmRunner&) = delete;
    VmRunner(VmRunner&&) = delete;
    VmRunner& operator=(VmRunner&&) = delete;

    // The machine itself — where its state is read. Reaching it is the stepping thread's privilege: on
    // a threaded runner that means from beforeFirstStep(), from the after-step hook, or once finished()
    // reports true.
    [[nodiscard]] Vm&       machine() noexcept { return *machine_; }
    [[nodiscard]] const Vm& machine() const noexcept { return *machine_; }

    [[nodiscard]] StepKind      kind() const noexcept { return kind_; }
    [[nodiscard]] Mode          mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint64_t cyclesPerStep() const noexcept { return cyclesPerStep_; }

    // Queue a gesture for the next step. Returns false when the mailbox is full — the gesture is
    // refused, and enqueueing never blocks. Throws std::logic_error on a started machine: a gesture is
    // performed through a resident machine's declared entries, and a started driver has none.
    bool enqueue(const Instruction& gesture);

    // Advance the machine by one step, then run the hook. Returns the CPU cycles the step consumed —
    // for a resident machine the gestures plus the tick call, the idle padding excluded.
    std::uint64_t stepOnce();

    // Advance the machine by a step worth `cycles` rather than the runner's own budget. A machine
    // driven by the engine's tick steps through this: what one tick is worth depends on the period
    // being run and the speed factor, so it varies from step to step.
    std::uint64_t stepOnce(std::uint64_t cycles);

    // Install what runs after each step. Replaces any hook already installed. Install it before the
    // machine is given a thread.
    void afterEachStep(std::function<void()> hook);

    // Install what runs at the top of each step, before the machine advances — where work that must
    // land at a step boundary is taken delivery of (a resident machine's gesture drain is this same
    // moment, built in). Replaces any hook already installed. Install it before the machine is given
    // a thread.
    void beforeEachStep(std::function<void()> hook);

    // CPU cycles the machine has run across every step so far — what stepOnce returned, summed. A
    // pacing closure reads it to answer how far the machine has run ahead of what it owes.
    [[nodiscard]] std::uint64_t cyclesRun() const noexcept {
        return cyclesRun_.load(std::memory_order_relaxed);
    }

    // Place the machine's content — its images, its routine, its audio sink. Placement mutates the
    // machine, so it happens on the thread that steps it: on the calling thread here for an inline
    // runner, and as the first act of the loop for a threaded one. Resolving the bytes that placement
    // consumes mutates nothing and stays with the caller. Install it before the machine is given a
    // thread.
    void beforeFirstStep(std::function<void()> work);

    // Give the machine its own thread. `backlog` reports how much of what the machine produced is still
    // waiting downstream, and the runner steps only while that is under `highWater` — so a machine runs
    // ahead by a bounded amount and its consumer paces it. `untilNext` answers how long until the next
    // step is due; supply it and a park lasts that or kParkInterval, whichever is shorter, so a step
    // lands when it is owed instead of at the next interval. It answers from the same numbers `backlog`
    // reports on, which is what keeps the two from disagreeing about whether it is time to step.
    // Threaded runners only, called once.
    void start(std::function<std::size_t()> backlog, std::size_t highWater,
               std::function<std::chrono::nanoseconds()> untilNext = {});

    // Ask the loop to leave. Wait-free: it neither blocks nor joins, so closing a machine never hands a
    // stalled one the power to stall the thread doing the closing.
    void requestStop() noexcept;

    // Whether the loop has left — always true of an inline runner. Destroying a runner that reports
    // true costs nothing; destroying one that reports false waits out the step in flight.
    [[nodiscard]] bool finished() const noexcept;

    // Wake a parked runner. Harmless on an inline one.
    void wake() noexcept;

private:
    // The threaded loop: place the content, then step while the backlog allows and park when it does
    // not, until asked to leave.
    void loop();

    std::optional<Vm>                  owned_;    // the owning form's machine; empty when borrowing
    Vm*                                machine_;  // the machine driven — owned_ or the borrowed one
    StepKind                           kind_;
    Mode                               mode_;
    std::uint64_t                      cyclesPerStep_;
    audio::SpscRingBuffer<Instruction> mailbox_;
    std::vector<Instruction>           drained_;  // the mailbox's landing buffer, sized once
    std::function<void()>              beforeStep_;
    std::function<void()>              afterStep_;
    std::function<void()>              place_;    // threaded only — the loop's first act
    std::atomic<std::uint64_t>         cyclesRun_{0};

    std::function<std::size_t()>             backlog_;
    std::function<std::chrono::nanoseconds()> untilNext_;  // empty: a park runs the full interval
    std::size_t                              highWater_ = 0;
    std::thread                  thread_;
    std::mutex                   mtx_;
    std::condition_variable      cv_;
    std::atomic<bool>            stop_{false};
    std::atomic<bool>            finished_{true};  // an inline runner is never inside a loop
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_RUNNER_H
