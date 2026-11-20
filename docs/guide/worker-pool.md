# Worker pool — a batch handed over and collected

`worker_pool.h`

`WorkerPool` is a pool of worker threads, sized once from the machine, that runs a batch of independent
jobs. A tick hands the batch over at one point and collects it at another; between the two the workers
run the jobs while the main thread goes on with its own work. A game makes one pool at startup, beside
its `RunLoop` and `Renderer`, and hands it to whatever steps work in parallel.

```cpp
#include "retropp/worker_pool.h"
```

## Contents

- [The two points in the tick](#the-two-points-in-the-tick)
- [The surface](#the-surface)
- [Examples](#examples)
- [What a job may touch](#what-a-job-may-touch)
- [The job's type](#the-jobs-type)
- [Inside a job](#inside-a-job)
- [A job that throws](#a-job-that-throws)
- [A batch's lifetime](#a-batchs-lifetime)
- [How many workers](#how-many-workers)
- [Related pages & where to change things](#related-pages--where-to-change-things)

## The two points in the tick

A batch is collected at the top of a tick and submitted at the bottom of it. The tick collects the
previous batch's results, applies them on the main thread in a fixed order, runs its own logic, takes a
snapshot of what each job will read, and submits the next batch. The workers then run that batch while
the main thread renders and paces, and the next tick collects it. When the jobs take less time than the
render, `collect` finds them finished and costs one atomic read.

The result is the result of stepping inline. Each job reads the snapshot and writes only its own output.
The main thread is the only thread that writes shared state, and it applies the outputs in index order.
Every worker count produces the same state, and so does a pool with no workers at all.

## The surface

```cpp
class WorkerPool {
public:
    static constexpr std::size_t kJobBytes = 64;                 // the room a batch has for its job

    static inline std::optional<std::size_t> defaultWorkers{};   // seeded by EngineConfig::setActive

    explicit WorkerPool(Platform& platform);                     // defaultWorkers, or the count the machine gives
    WorkerPool(Platform& platform, std::size_t workers);         // exactly this many, zero included
    ~WorkerPool();                                               // finishes outstanding batches, then joins

    [[nodiscard]] std::size_t workers() const noexcept;          // 0: batches run at collect()

    class Batch {
    public:
        Batch() noexcept;                                        // empty
        ~Batch();                                                // finishes the batch; drops its exception
        Batch(Batch&&) noexcept;                                 // the source is left empty
        Batch& operator=(Batch&&) noexcept;                      // finishes the batch held, then takes the other

        [[nodiscard]] bool done() const noexcept;                // every job has run; true for an empty Batch
        void collect();                                          // helps, waits, rethrows; then empty
    };

    template <class Job>
    [[nodiscard]] Batch submit(std::size_t count, Job&& job);    // job(0) … job(count - 1), each once
};
```

A pool cannot be copied or moved. A `Batch` can be moved but not copied.

**`submit(count, job)`** runs `job(0)` through `job(count − 1)`. Each index runs exactly once, on any
thread and in any order, and jobs of one batch may run at the same moment. The call places the job in
the batch's `kJobBytes` of storage, wakes one worker when the pool has any, and returns. It builds
nothing per job, so it costs the calling thread the same for one job as for a million. It runs no job
itself. A `count` of 0 returns an empty `Batch`. Once a pool has held N batches at the same time, handing
over and collecting up to N at a time allocates nothing.

**`collect()`** returns once every job has run, and everything the jobs wrote is then visible to the
caller with no further synchronization. A finished batch costs one atomic read, with no lock and no wait.
On an unfinished batch the calling thread first runs the jobs of that batch that no thread has taken yet,
then waits on a condition variable for the ones still running. It runs jobs of its own batch only. Once
`collect` returns, the `Batch` is empty and a second `collect` does nothing.

**`done()`** answers whether every job has run, without running or waiting for anything. It is true for
an empty `Batch`.

**`workers()`** is how many worker threads the pool started. On a pool with no workers, every job of a
batch runs on the thread that calls `collect`.

Between batches a worker waits on a condition variable. An idle pool uses no CPU.

## Examples

**Startup and the tick.**

```cpp
const EngineConfig config{
    .identity = {.organization = "Zorlduh", .application = "Zorlduh"}};
EngineConfig::setActive(config);

SteadyClock clock;
SdlPlatform platform;
Renderer    renderer{platform.device(), platform.sdlWindow()};
RunLoop     loop{clock};
WorkerPool  pool{platform};                  // one per program, made beside the others
WorkerPool::Batch stepping;                  // empty: done() is true, collect() does nothing

loop.simTick([&](const InputState& input) {
    stepping.collect();                      // last tick's steps; normally already finished
    applyRequests(state);                    // main thread, fixed order: the only writes

    // … the tick's own logic …

    gatherValues(state);                     // snapshot what each processor will read
    stepping = pool.submit(processors.size(),
                           [&](std::size_t i) { processors[i].step(); });
});                                          // the workers step while the main thread renders and paces
```

A catch-up burst, where one `RunLoop::advance()` runs several ticks, needs nothing extra. The next tick's
`collect` finds the batch unfinished and runs the jobs no worker has taken yet on the calling thread.

**The same game inline, to compare.**

```cpp
const EngineConfig config{
    .identity = {.organization = "Zorlduh", .application = "Zorlduh"},
    .workers  = 0};                          // every pool made without a count runs its batches at collect()

WorkerPool inlinePool{platform, 0};          // or for one pool only
```

**How many workers it got, and whether a batch is finished.**

```cpp
log("workers: {}", pool.workers());          // 0 on one core
if (stepping.done()) { /* collect() will not run or wait for anything */ }
```

**A job that throws.**

```cpp
auto batch = pool.submit(scripts.size(), [&](std::size_t i) { scripts[i].run(); });
try {
    batch.collect();                         // every other index still ran
} catch (const ScriptError& error) {         // the lowest throwing index's exception, at any worker count
    report(error);
}
```

**A test that fixes the core count.**

```cpp
struct FourCores final : MyTestPlatform {    // the game's own Platform for its tests
    std::size_t coreCount() const override { return 4; }
};
FourCores  platform;
WorkerPool pool{platform};                   // 3 workers on any machine the test runs on
```

**Shutdown.** Nothing to call. A `Batch` going out of scope finishes its jobs. The pool going out of
scope finishes every batch still outstanding, then joins its workers.

## What a job may touch

A job touches only what its caller gave it: a snapshot it reads and nothing writes while the batch is
outstanding, and an output that belongs to its own index. Two jobs of one batch can run at the same
moment, so a job that writes anything another job reads or writes is a data race. Shared state is
written by the main thread, from the jobs' outputs, after `collect`.

## The job's type

`submit` checks the job's type as the program compiles, and a job that fails a check does not compile.
The compiler's message names the rule:

- **It is called as `job(index)` through a `const` reference**, on several threads at once. A `mutable`
  lambda does not qualify.
- **It fits `kJobBytes`** (64 bytes). Capture a pointer or a reference to what it reads, not the data
  itself.
- **Its alignment is at most `alignof(std::max_align_t)`.**
- **It is trivially destructible.** Capture by reference or by pointer; a capture of a `std::string` or
  a `std::vector` by value has a destructor to run.

A lambda that captures by reference, as in the examples above, meets all four.

## Inside a job

A job runs what it was handed. `submit` called from inside a job throws `std::logic_error`, and so does
`collect` on any `Batch`. Like any exception a job throws, it surfaces at the outer batch's `collect`.

## A job that throws

A job that throws does not stop the others: every index of the batch still runs once. `collect`
rethrows the exception of the lowest index that threw, so the exception a caller sees is the same at
every worker count. The `Batch` is then empty, and a second `collect` does nothing.

## A batch's lifetime

- A `Batch` that goes out of scope uncollected finishes its jobs in its destructor and drops any
  exception they threw.
- Move-assigning over a `Batch` that holds a batch finishes that batch first. A moved-from `Batch` is
  empty.
- A `Batch` stays valid after its pool is gone. The pool's destructor ran its jobs, so `collect`
  answers at once, and rethrows what a job threw.
- Several batches may be outstanding at once, submitted from different threads; `submit` is safe from
  any thread that is not inside a job. One `Batch` is used from one thread at a time.

## How many workers

A pool given a count has that count, zero included. A pool made without one takes
`WorkerPool::defaultWorkers` when it is set, which `EngineConfig::setActive` seeds from
`EngineConfig::workers`. Otherwise the machine decides the count:

| `platform.coreCount()` | Workers |
|---|---|
| 0 (unknown) or 1 | 0 |
| 2 or more | `cores − 1 − machines`, never fewer than 1 |

`machines` is the number of machines running on a thread of their own at the moment the pool is made;
the pool reads it once, at construction. On eight cores with one
machine running, a pool gets six workers. On two cores it gets one, whatever is running.

Every worker starts through `platform.startThread`, and the platform is used during construction only
([platform-and-windowing.md](platform-and-windowing.md) § The seam).

## Related pages & where to change things

- [run-loop-and-timing.md](run-loop-and-timing.md) — the tick a batch is collected and submitted in.
- [platform-and-windowing.md](platform-and-windowing.md) — `coreCount()` and `startThread()` on the
  `Platform` seam, and `EngineConfig::workers` under the startup configuration.
- The public header is `include/retropp/worker_pool.h`; the implementation is `src/worker_pool.cpp`.
