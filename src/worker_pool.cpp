#include "retropp/worker_pool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "retropp/platform.h"
#include "src/vm/machine_threads.h"
#include "src/worker_pool_testing.h"

namespace retropp {

namespace detail {

// One batch's place in the pool. The next submit refills a slot as soon as its handle has let it go, so
// a pool that has held N batches at once holds up to N again without allocating. Every atomic here uses
// the default, sequentially consistent ordering: a collector that announces it is about to wait and the
// participant that finishes the last job each read what the other wrote, so one of them always sees the
// other and the wake is never missed.
struct WorkerPoolSlot {
    // How many of the batch's indices nobody has taken; zero once every index is taken. It is the one
    // field of the slot a participant reads before it holds an index, and submit stores it last, so a
    // participant that takes an index reads the batch that index belongs to.
    alignas(64) std::atomic<std::size_t> untaken{0};

    // The jobs not finished yet. It reaches zero exactly once per batch, after the last job's writes.
    alignas(64) std::atomic<std::size_t> remaining{0};

    alignas(64) std::size_t count = 0;
    alignas(std::max_align_t) std::byte job[WorkerPool::kJobBytes]{};
    void (*run)(const void* job, std::size_t index) = nullptr;  // calls the job placed in `job`

    // The exception of the lowest index that threw, written under the pool's mutex before that job is
    // counted finished, so a collector that reads `remaining` as zero reads it with no lock.
    std::exception_ptr error;
    std::size_t        errorIndex = 0;

    std::atomic<bool> letGo{true};  // the handle has finished with it; the next submit may refill it
};

struct WorkerPoolState {
    std::mutex              mutex;
    std::condition_variable workersWake;     // the workers park on it between batches
    std::condition_variable collectorsWake;  // a collector waits on it for the jobs in flight

    // The batches handed over with an index still untaken, oldest first. A submit drops the ones whose every
    // index is taken; capacity is kept, so a warm pool grows it no further.
    std::vector<WorkerPoolSlot*> outstanding;

    // Every slot ever made. A slot is made only when every existing one is in use and is never moved.
    std::vector<std::unique_ptr<WorkerPoolSlot>> slots;

    std::vector<std::thread> threads;
    bool                     leaving = false;

    std::atomic<std::size_t>   collectorsWaiting{0};  // collectors about to wait, or waiting
    std::atomic<std::uint64_t> looks{0};              // each evaluation of a worker's wait
};

}  // namespace detail

namespace {

using detail::WorkerPoolSlot;
using detail::WorkerPoolState;

// Set around every job call on every participant, so a submit or a collect from inside a job is refused.
thread_local bool insideJob = false;

// The oldest outstanding batch with an index nobody has taken, or null. Under the pool's mutex.
WorkerPoolSlot* firstUnclaimed(WorkerPoolState& state) {
    for (WorkerPoolSlot* slot : state.outstanding) {
        if (slot->untaken.load() != 0) {
            return slot;
        }
    }
    return nullptr;
}

// Runs one index: the job, an exception recorded if it is the lowest so far, then the count. The
// participant whose count takes the batch to zero wakes the collectors when one is waiting; taking the
// mutex first means a collector between its last look and its wait cannot miss the wake.
void runIndex(WorkerPoolState& state, WorkerPoolSlot& slot, std::size_t index) {
    const bool outer = insideJob;
    insideJob        = true;
    try {
        slot.run(slot.job, index);
    } catch (...) {
        const std::lock_guard<std::mutex> lock(state.mutex);
        if (!slot.error || index < slot.errorIndex) {
            slot.error      = std::current_exception();
            slot.errorIndex = index;
        }
    }
    insideJob = outer;
    if (slot.remaining.fetch_sub(1) == 1 && state.collectorsWaiting.load() != 0) {
        { const std::lock_guard<std::mutex> lock(state.mutex); }
        state.collectorsWake.notify_all();
    }
}

// Takes and runs indices of the batch in `slot` until none is left. A take is one compare-exchange of
// `untaken` from n to n − 1; the index it gives is count − n, so indices go out in ascending order. Until
// a take succeeds nothing of the slot is read but `untaken`. After it, the slot holds that index's batch
// until the index is counted finished, since the handle lets the slot go only once `remaining` is zero.
// A participant that reaches a slot after its batch was collected and the slot refilled either reads
// zero or takes an index of the batch now there, and runs it like any other. A worker whose take leaves
// more indices untaken wakes one more worker, once per visit, so a batch draws in as many workers as it
// has jobs.
void runClaims(WorkerPoolState& state, WorkerPoolSlot& slot, bool wakeAnother) {
    std::size_t untaken = slot.untaken.load();
    for (;;) {
        if (untaken == 0) {
            return;
        }
        if (!slot.untaken.compare_exchange_weak(untaken, untaken - 1)) {
            continue;
        }
        const std::size_t index = slot.count - untaken;
        if (wakeAnother && untaken > 1) {
            state.workersWake.notify_one();
            wakeAnother = false;
        }
        runIndex(state, slot, index);
        untaken = slot.untaken.load();
    }
}

// A worker: park until a batch has an untaken index or the pool is leaving, run that batch's indices,
// look again. It leaves once the pool is leaving and no index is left to take.
void workerLoop(WorkerPoolState& state) {
    std::unique_lock<std::mutex> lock(state.mutex);
    for (;;) {
        WorkerPoolSlot* slot = nullptr;
        state.workersWake.wait(lock, [&] {
            ++state.looks;
            slot = firstUnclaimed(state);
            return slot != nullptr || state.leaving;
        });
        if (slot == nullptr) {
            return;
        }
        lock.unlock();
        runClaims(state, *slot, true);
        lock.lock();
    }
}

// The worker count by the machine: none on one core, otherwise one fewer than the cores less the
// machines on their own threads, never fewer than one.
std::size_t countFromTheMachine(const Platform& platform) {
    const std::size_t cores    = platform.coreCount();
    const std::size_t machines = vm::machineThreads.load();
    if (cores <= 1) {
        return 0;
    }
    const std::size_t spare = cores - 1;
    return machines >= spare ? 1 : spare - machines;
}

}  // namespace

// ── WorkerPool ──────────────────────────────────────────────────────────────────────────────────────

WorkerPool::WorkerPool(Platform& platform)
    : WorkerPool(platform, defaultWorkers ? *defaultWorkers : countFromTheMachine(platform)) {}

WorkerPool::WorkerPool(Platform& platform, std::size_t workers)
    : state_(std::make_shared<WorkerPoolState>()) {
    WorkerPoolState* state = state_.get();
    state->threads.reserve(workers);
    try {
        for (std::size_t i = 0; i < workers; ++i) {
            state->threads.push_back(platform.startThread([state] { workerLoop(*state); }));
        }
    } catch (...) {
        // A thread the host could not start: the ones already running leave and are joined before the
        // failure reaches the caller.
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->leaving = true;
        }
        state->workersWake.notify_all();
        for (std::thread& thread : state->threads) {
            thread.join();
        }
        throw;
    }
}

WorkerPool::~WorkerPool() {
    WorkerPoolState& state = *state_;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        state.leaving = true;
    }
    state.workersWake.notify_all();

    // Every outstanding batch's untaken indices run here, taken the way a worker takes them, oldest batch
    // first; the workers finish the ones they hold before they leave, so after the joins every job has run.
    std::unique_lock<std::mutex> lock(state.mutex);
    while (WorkerPoolSlot* slot = firstUnclaimed(state)) {
        lock.unlock();
        runClaims(state, *slot, false);
        lock.lock();
    }
    lock.unlock();

    for (std::thread& thread : state.threads) {
        thread.join();
    }
}

std::size_t WorkerPool::workers() const noexcept { return state_->threads.size(); }

WorkerPool::Batch WorkerPool::submitStored(std::size_t count, void* job, PlaceJob place, RunJob run) {
    if (insideJob) {
        throw std::logic_error(
            "WorkerPool::submit: called from inside a job — a job runs what it was handed and submits "
            "nothing");
    }
    if (count == 0) {
        return Batch{};
    }

    WorkerPoolState& state = *state_;
    WorkerPoolSlot*  slot  = nullptr;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        std::erase_if(state.outstanding,
                      [](const WorkerPoolSlot* s) { return s->untaken.load() == 0; });
        for (const std::unique_ptr<WorkerPoolSlot>& s : state.slots) {
            if (s->letGo.load()) {
                slot = s.get();
                break;
            }
        }
        if (slot == nullptr) {
            slot = state.slots.emplace_back(std::make_unique<WorkerPoolSlot>()).get();
        }
        // Everything a participant reads after taking an index goes in first; the store of `untaken`
        // is what hands the batch over.
        place(slot->job, job);
        slot->run        = run;
        slot->count      = count;
        slot->error      = nullptr;
        slot->errorIndex = 0;
        slot->remaining.store(count);
        slot->letGo.store(false);
        slot->untaken.store(count);
        state.outstanding.push_back(slot);
    }
    if (!state.threads.empty()) {
        state.workersWake.notify_one();
    }
    return Batch{state_, slot};
}

// ── WorkerPool::Batch ───────────────────────────────────────────────────────────────────────────────

WorkerPool::Batch::Batch() noexcept = default;

WorkerPool::Batch::Batch(std::shared_ptr<detail::WorkerPoolState> state,
                         detail::WorkerPoolSlot*                  slot) noexcept
    : state_(std::move(state)), slot_(slot) {}

WorkerPool::Batch::~Batch() {
    if (slot_ != nullptr) {
        finish(false);
    }
}

WorkerPool::Batch::Batch(Batch&& other) noexcept
    : state_(std::move(other.state_)), slot_(std::exchange(other.slot_, nullptr)) {}

WorkerPool::Batch& WorkerPool::Batch::operator=(Batch&& other) noexcept {
    if (this != &other) {
        if (slot_ != nullptr) {
            finish(false);
        }
        state_ = std::move(other.state_);
        slot_  = std::exchange(other.slot_, nullptr);
    }
    return *this;
}

bool WorkerPool::Batch::done() const noexcept { return slot_ == nullptr || slot_->remaining.load() == 0; }

void WorkerPool::Batch::collect() {
    if (insideJob) {
        throw std::logic_error(
            "WorkerPool::Batch::collect: called from inside a job — a batch is collected outside any job");
    }
    if (slot_ == nullptr) {
        return;
    }
    finish(true);
}

void WorkerPool::Batch::finish(bool rethrow) {
    WorkerPoolState& state = *state_;
    WorkerPoolSlot&  slot  = *slot_;
    if (slot.remaining.load() != 0) {
        runClaims(state, slot, false);
        if (slot.remaining.load() != 0) {
            ++state.collectorsWaiting;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.collectorsWake.wait(lock, [&] { return slot.remaining.load() == 0; });
            }
            --state.collectorsWaiting;
        }
    }
    std::exception_ptr error = slot.error;
    slot.letGo.store(true);
    slot_ = nullptr;
    state_.reset();
    if (rethrow && error) {
        std::rethrow_exception(error);
    }
}

// ── The test seam ───────────────────────────────────────────────────────────────────────────────────

std::unique_lock<std::mutex> detail::WorkerPoolTestAccess::holdLock(WorkerPool& pool) {
    return std::unique_lock<std::mutex>(pool.state_->mutex);
}

std::uint64_t detail::WorkerPoolTestAccess::looks(const WorkerPool& pool) {
    return pool.state_->looks.load();
}

}  // namespace retropp
