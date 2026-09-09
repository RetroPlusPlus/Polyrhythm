#include "src/vm/vm_runner.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

namespace retropp::vm {

VmRunner::VmRunner(Vm machine, StepKind kind, std::uint64_t cyclesPerStep, Mode mode)
    : owned_(std::move(machine)),
      machine_(&*owned_),
      kind_(kind),
      mode_(mode),
      cyclesPerStep_(cyclesPerStep),
      mailbox_(kMailboxCapacity),
      // The landing buffer is sized once and reused: a drain writes over its front and the step reads
      // the count the drain returned, so no step allocates.
      drained_(kMailboxCapacity) {}

VmRunner::VmRunner(Vm* machine, StepKind kind, std::uint64_t cyclesPerStep, Mode mode)
    : machine_(machine),
      kind_(kind),
      mode_(mode),
      cyclesPerStep_(cyclesPerStep),
      mailbox_(kMailboxCapacity),
      drained_(kMailboxCapacity) {}

VmRunner::~VmRunner() {
    requestStop();
    wake();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool VmRunner::enqueue(const Instruction& gesture) {
    if (kind_ != StepKind::Resident) {
        throw std::logic_error(
            "VmRunner::enqueue: a gesture is performed through a resident machine's declared entries — "
            "this runner drives a started driver, which has none");
    }
    return mailbox_.push(gesture);
}

std::uint64_t VmRunner::stepOnce() { return stepOnce(cyclesPerStep_); }

std::uint64_t VmRunner::stepOnce(std::uint64_t cycles) {
    if (beforeStep_) {
        beforeStep_();
    }
    std::uint64_t spent = 0;
    if (kind_ == StepKind::Resident) {
        // Drain first: the gestures this step performs are the ones already offered, and a gesture
        // arriving during the step waits for the next one.
        const std::size_t queued = mailbox_.pop(std::span<Instruction>(drained_));
        spent = machine_->tickDriver(std::span<const Instruction>(drained_.data(), queued), cycles);
    } else {
        spent = machine_->stepDriver(cycles);
    }
    cyclesRun_.fetch_add(spent, std::memory_order_relaxed);
    if (afterStep_) {
        afterStep_();
    }
    return spent;
}

void VmRunner::afterEachStep(std::function<void()> hook) { afterStep_ = std::move(hook); }

void VmRunner::beforeEachStep(std::function<void()> hook) { beforeStep_ = std::move(hook); }

void VmRunner::beforeFirstStep(std::function<void()> work) {
    if (mode_ == Mode::Inline) {
        work();  // the calling thread is the stepping thread — placement happens here and now
        return;
    }
    place_ = std::move(work);
}

void VmRunner::start(std::function<std::size_t()> backlog, std::size_t highWater,
                     std::function<std::chrono::nanoseconds()> untilNext) {
    backlog_   = std::move(backlog);
    untilNext_ = std::move(untilNext);
    highWater_ = highWater;
    finished_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop(); });
}

void VmRunner::requestStop() noexcept { stop_.store(true, std::memory_order_relaxed); }

bool VmRunner::finished() const noexcept { return finished_.load(std::memory_order_acquire); }

void VmRunner::wake() noexcept {
    {
        std::lock_guard<std::mutex> lock(mtx_);  // order the state the waker changed before the
    }                                            // loop's next look at it
    cv_.notify_one();
}

void VmRunner::loop() {
    if (place_) {
        place_();
    }
    for (;;) {
        if (stop_.load(std::memory_order_relaxed)) {
            break;
        }
        if (!backlog_ || backlog_() < highWater_) {
            stepOnce();
            continue;
        }
        // Asked before the lock is taken, so the deadline is read on the same terms the backlog was
        // and no closure runs under the mutex.
        const std::chrono::nanoseconds park =
            untilNext_ ? std::min<std::chrono::nanoseconds>(untilNext_(), kParkInterval)
                       : std::chrono::nanoseconds{kParkInterval};
        std::unique_lock<std::mutex> lock(mtx_);
        if (stop_.load(std::memory_order_relaxed)) {
            break;
        }
        cv_.wait_for(lock, park);
    }
    finished_.store(true, std::memory_order_release);
}

}  // namespace retropp::vm
