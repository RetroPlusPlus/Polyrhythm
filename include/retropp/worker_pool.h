#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>

namespace retropp {

class Platform;

namespace detail {
struct WorkerPoolState;
struct WorkerPoolSlot;
class WorkerPoolTestAccess;
}  // namespace detail

// A pool of worker threads, sized once from the machine, that runs a batch of jobs handed over at one
// point in the tick and collected at another. A game makes one at startup, beside its RunLoop and
// Renderer, and hands it to whatever steps work in parallel.
//
//     WorkerPool        pool{platform};
//     WorkerPool::Batch stepping;
//
//     loop.simTick([&](const InputState& input) {
//         stepping.collect();               // last tick's steps; normally already finished
//         applyRequests(state);             // the only writes, on this thread, in a fixed order
//         gatherValues(state);              // the snapshot each step reads
//         stepping = pool.submit(processors.size(),
//                                [&](std::size_t i) { processors[i].step(); });
//     });
//
// submit() runs job(0) … job(count − 1), each exactly once, on any thread and in any order, and returns
// at once; its cost on the calling thread is the same for one job or a million. collect() makes every
// job's writes visible to the caller. A job touches only what its caller gave it, so the result is the
// result of a plain loop at every worker count, zero included. See docs/guide/worker-pool.md.
class WorkerPool {
public:
    // The room a batch has for its job — the callable itself, not what it points at.
    static constexpr std::size_t kJobBytes = 64;

    // The worker count of a pool made without one. EngineConfig::setActive seeds it from
    // EngineConfig::workers; unset, the machine decides.
    static inline std::optional<std::size_t> defaultWorkers{};

    // A pool of defaultWorkers workers when it is set. Otherwise the machine decides the count:
    // 0 when platform.coreCount() is 0 or 1, and otherwise one fewer than the cores less the machines
    // running on their own threads as the pool is made, never fewer than 1. Every worker starts
    // through platform.startThread; the platform is used here and nowhere after.
    explicit WorkerPool(Platform& platform);

    // A pool of exactly `workers` workers, zero included.
    WorkerPool(Platform& platform, std::size_t workers);

    // Runs every outstanding batch's unclaimed jobs on this thread, waits for the ones in flight, then
    // joins the workers. A Batch still held stays valid.
    ~WorkerPool();

    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&)                 = delete;
    WorkerPool& operator=(WorkerPool&&)      = delete;

    // How many workers the pool started. At 0 every batch runs on the thread that collects it.
    [[nodiscard]] std::size_t workers() const noexcept;

    // One submitted batch: the handle that collects it. Move-only; used from one thread at a time.
    class Batch {
    public:
        // An empty batch: done() is true and collect() returns at once.
        Batch() noexcept;

        // Finishes the batch the way collect() does, and drops any exception a job threw.
        ~Batch();

        // The source is left empty.
        Batch(Batch&& other) noexcept;

        // Finishes the batch held, dropping any exception, then takes the other's.
        Batch& operator=(Batch&& other) noexcept;

        Batch(const Batch&)            = delete;
        Batch& operator=(const Batch&) = delete;

        // Whether every job has run; true for an empty Batch. One atomic read.
        [[nodiscard]] bool done() const noexcept;

        // Returns once every job has run, with everything the jobs wrote visible here. A finished batch
        // costs one atomic read and no lock. An unfinished one has this thread run the batch's jobs no
        // thread has taken yet, then wait for the ones in flight. If any job threw, rethrows the
        // exception of the lowest index that threw — every other index still ran. The Batch is empty
        // afterward, so a second collect() does nothing. Called from inside a job, throws
        // std::logic_error.
        void collect();

    private:
        friend class WorkerPool;
        Batch(std::shared_ptr<detail::WorkerPoolState> state, detail::WorkerPoolSlot* slot) noexcept;

        // Runs, waits, empties the handle; rethrows a job's exception only when asked to.
        void finish(bool rethrow);

        std::shared_ptr<detail::WorkerPoolState> state_;
        detail::WorkerPoolSlot*                  slot_ = nullptr;
    };

    // Hands over job(0) … job(count − 1) and returns at once. The job is placed in the batch's
    // kJobBytes and one worker is woken; it builds nothing per job and runs no job itself. A count of
    // 0 returns an empty Batch. Once a pool has held N batches at the same time, submit and collect
    // allocate nothing for up to N at a time. Called from inside a job, throws std::logic_error.
    //
    // The job is checked at compile time: it is called as job(index) through a const reference on
    // several threads at once, so a mutable lambda does not compile; it fits kJobBytes and
    // std::max_align_t; it is trivially destructible. A lambda capturing by reference or by pointer
    // meets all four.
    template <class Job>
    [[nodiscard]] Batch submit(std::size_t count, Job&& job);

private:
    friend class detail::WorkerPoolTestAccess;

    using PlaceJob = void (*)(void* storage, void* job);
    using RunJob   = void (*)(const void* job, std::size_t index);

    // The library half of submit(): the inside-a-job check, the slot, the wake.
    Batch submitStored(std::size_t count, void* job, PlaceJob place, RunJob run);

    std::shared_ptr<detail::WorkerPoolState> state_;
};

template <class Job>
WorkerPool::Batch WorkerPool::submit(std::size_t count, Job&& job) {
    using Stored = std::decay_t<Job>;
    constexpr bool kCallable     = std::is_invocable_v<const Stored&, std::size_t>;
    constexpr bool kFits         = sizeof(Stored) <= kJobBytes;
    constexpr bool kAligned      = alignof(Stored) <= alignof(std::max_align_t);
    constexpr bool kDestructible = std::is_trivially_destructible_v<Stored>;
    static_assert(kCallable,
                  "WorkerPool::submit: the job is called as job(index) through a const reference — a "
                  "mutable lambda does not qualify");
    static_assert(kFits,
                  "WorkerPool::submit: the job holds more than kJobBytes — capture a pointer or a "
                  "reference to what it reads");
    static_assert(kAligned, "WorkerPool::submit: the job's alignment is past std::max_align_t");
    static_assert(kDestructible,
                  "WorkerPool::submit: the job must be trivially destructible — capture by reference or "
                  "by pointer");
    if constexpr (kCallable && kFits && kAligned && kDestructible) {
        return submitStored(
            count, const_cast<void*>(static_cast<const void*>(std::addressof(job))),
            [](void* storage, void* source) {
                ::new (storage) Stored(static_cast<Job&&>(*static_cast<std::remove_reference_t<Job>*>(source)));
            },
            [](const void* stored, std::size_t index) {
                (*std::launder(static_cast<const Stored*>(stored)))(index);
            });
    } else {
        return Batch{};
    }
}

}  // namespace retropp
