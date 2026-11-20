// WorkerPool — a batch handed over at one point and collected at another. Every pool here runs on a
// MockPlatform, so its worker count is the same on any machine the suite runs on. Every wait a defect
// could turn into a hang is bounded at five seconds and fails.

#include "retropp/worker_pool.h"

#include "mock_platform.h"
#include "retropp/engine_config.h"
#include "src/vm/machine_threads.h"
#include "src/worker_pool_testing.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kBound = 5s;

// Wait for something another thread brings about, up to the bound. It returns what it saw rather than
// sleeping a fixed span, so a case fails on the condition it names.
template <typename Predicate>
bool waitUntil(Predicate done, std::chrono::milliseconds limit = kBound) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

// Run `step` on a thread of its own and wait for it up to the bound. A step that does not come back
// fails the case and ends the process: what it waits for will not arrive, and nothing after it can run.
// An exception the step throws is rethrown here.
template <typename Step>
void withinBound(const char* what, Step step) {
    std::atomic<bool>  returned{false};
    std::exception_ptr error;
    std::thread        thread([&] {
        try {
            step();
        } catch (...) {
            error = std::current_exception();
        }
        returned = true;
    });
    if (!waitUntil([&] { return returned.load(); })) {
        ADD_FAILURE() << what << " did not return within " << kBound.count() << " ms";
        std::fflush(stdout);
        std::_Exit(1);
    }
    thread.join();
    if (error) {
        std::rethrow_exception(error);
    }
}

// collect() through withinBound.
void collectWithinBound(WorkerPool::Batch& batch) {
    withinBound("collect()", [&] { batch.collect(); });
}

// A job body that holds its thread until `gate` opens or the bound passes.
void holdUntil(const std::atomic<bool>& gate) {
    waitUntil([&] { return gate.load(); });
}

// The process-wide inputs to a pool's count, set for one case and put back after it: the machines on
// their own threads, and the active config (setActive of a config that sets no worker count).
class CountInputs {
public:
    CountInputs() : machines_(vm::machineThreads.load()) {
        vm::machineThreads = 0;
        EngineConfig::setActive(identified());
    }
    ~CountInputs() {
        vm::machineThreads = machines_;
        EngineConfig::setActive(identified());
    }
    CountInputs(const CountInputs&)            = delete;
    CountInputs& operator=(const CountInputs&) = delete;

    static EngineConfig identified() {
        EngineConfig config{};
        config.identity = {.organization = "Retro++", .application = "WorkerPoolTest"};
        return config;
    }

private:
    std::size_t machines_;
};

// ── Each index once ───────────────────────────────────────────────────────────────────────────────

TEST(WorkerPool, EachIndexRunsExactlyOnce) {
    for (const std::size_t workers : {0u, 1u, 4u}) {
        test::MockPlatform platform{0};
        WorkerPool         pool{platform, workers};
        for (const std::size_t count : {0u, 1u, 1000u}) {
            std::vector<std::atomic<int>> runs(count);
            WorkerPool::Batch batch = pool.submit(count, [&runs](std::size_t i) { runs[i].fetch_add(1); });
            collectWithinBound(batch);
            for (std::size_t i = 0; i < count; ++i) {
                ASSERT_EQ(runs[i].load(), 1) << "index " << i << " of " << count << " at " << workers
                                             << " workers";
            }
        }
    }
}

// Each index writes its own slot from a snapshot nothing writes, so the vector a plain loop builds is the
// one every worker count builds.
TEST(WorkerPool, TheSameResultsAtEveryWorkerCount) {
    constexpr std::size_t kCount = 4096;
    std::vector<std::uint32_t> snapshot(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        snapshot[i] = static_cast<std::uint32_t>(i * 2654435761u);
    }
    const auto step = [](std::size_t i, const std::vector<std::uint32_t>& from) {
        std::uint32_t value = from[i] ^ from[(i * 7 + 3) % from.size()];
        for (int round = 0; round < 16; ++round) {
            value = value * 1664525u + 1013904223u;
        }
        return value;
    };

    std::vector<std::uint32_t> reference(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        reference[i] = step(i, snapshot);
    }

    for (const std::size_t workers : {0u, 1u, 2u, 8u}) {
        test::MockPlatform         platform{0};
        WorkerPool                 pool{platform, workers};
        std::vector<std::uint32_t> out(kCount);
        WorkerPool::Batch batch =
            pool.submit(kCount, [&out, &snapshot, &step](std::size_t i) { out[i] = step(i, snapshot); });
        collectWithinBound(batch);
        EXPECT_EQ(out, reference) << "at " << workers << " workers";
    }
}

// ── Handing over ──────────────────────────────────────────────────────────────────────────────────

// With no worker nothing claims, so submit returns having run nothing and collect runs it all.
TEST(WorkerPool, SubmitRunsNoJobOnAPoolWithNoWorkers) {
    test::MockPlatform       platform{0};
    WorkerPool               pool{platform, 0};
    std::atomic<std::size_t> ran{0};

    WorkerPool::Batch batch = pool.submit(1'000'000, [&ran](std::size_t) { ran.fetch_add(1); });
    EXPECT_EQ(ran.load(), 0u);
    EXPECT_FALSE(batch.done());

    collectWithinBound(batch);
    EXPECT_EQ(ran.load(), 1'000'000u);
    EXPECT_TRUE(batch.done());
}

// ── Collecting ────────────────────────────────────────────────────────────────────────────────────

// A finished batch answers with the pool's mutex held elsewhere: done() and collect() take no lock.
TEST(WorkerPool, CollectOfAFinishedBatchTakesNoLock) {
    test::MockPlatform        platform{0};
    WorkerPool                pool{platform, 1};
    std::array<std::uint8_t, 8> marks{};
    WorkerPool::Batch batch = pool.submit(marks.size(), [&marks](std::size_t i) { marks[i] = 1; });
    ASSERT_TRUE(waitUntil([&] { return batch.done(); }));

    std::unique_lock<std::mutex> held = detail::WorkerPoolTestAccess::holdLock(pool);
    std::atomic<bool>            answered{false};
    bool                         finished = false;
    std::thread                  other([&] {
        finished = batch.done();
        batch.collect();
        answered = true;
    });
    const bool inTime = waitUntil([&] { return answered.load(); });
    held.unlock();
    other.join();

    EXPECT_TRUE(inTime);
    EXPECT_TRUE(finished);
    EXPECT_EQ(marks, (std::array<std::uint8_t, 8>{1, 1, 1, 1, 1, 1, 1, 1}));
}

// The one worker is held inside another batch's job, so the second batch's jobs are all run by the
// thread that collects it.
TEST(WorkerPool, CollectRunsUnclaimedJobsOnTheCallingThread) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 1};
    std::atomic<bool>  entered{false};
    std::atomic<bool>  gate{false};
    WorkerPool::Batch  holding = pool.submit(1, [&entered, &gate](std::size_t) {
        entered = true;
        holdUntil(gate);
    });
    ASSERT_TRUE(waitUntil([&] { return entered.load(); }));

    std::vector<std::thread::id> ranOn(16);
    WorkerPool::Batch            second =
        pool.submit(ranOn.size(), [&ranOn](std::size_t i) { ranOn[i] = std::this_thread::get_id(); });
    std::thread::id collector;
    withinBound("collect() of the second batch", [&] {
        collector = std::this_thread::get_id();
        second.collect();
    });

    gate = true;
    collectWithinBound(holding);
    for (std::size_t i = 0; i < ranOn.size(); ++i) {
        EXPECT_EQ(ranOn[i], collector) << "index " << i;
    }
}

// A job the worker has claimed is still inside its call; collect does not return until it has finished.
TEST(WorkerPool, CollectWaitsForAJobInFlight) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 1};
    std::atomic<bool>  entered{false};
    std::atomic<bool>  gate{false};
    std::atomic<bool>  jobFinished{false};
    WorkerPool::Batch  batch = pool.submit(1, [&](std::size_t) {
        entered = true;
        holdUntil(gate);
        jobFinished = true;
    });
    ASSERT_TRUE(waitUntil([&] { return entered.load(); }));

    std::atomic<bool> returned{false};
    bool              finishedWhenReturned = false;
    std::thread       collector([&] {
        batch.collect();
        finishedWhenReturned = jobFinished.load();
        returned             = true;
    });
    std::this_thread::sleep_for(50ms);
    const bool returnedEarly = returned.load();

    gate = true;
    const bool returnedInTime = waitUntil([&] { return returned.load(); });
    collector.join();

    EXPECT_FALSE(returnedEarly);
    EXPECT_TRUE(returnedInTime);
    EXPECT_TRUE(finishedWhenReturned);
}

// Four jobs that each wait for all four to be in flight can only get there if the batch draws in all
// four workers — and nothing here calls collect to help.
TEST(WorkerPool, ABatchWakesAsManyWorkersAsItHasJobs) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 4};
    std::atomic<int>   inFlight{0};
    std::atomic<int>   sawAllFour{0};
    std::atomic<bool>  abandon{false};

    WorkerPool::Batch batch = pool.submit(4, [&](std::size_t) {
        inFlight.fetch_add(1);
        if (waitUntil([&] { return inFlight.load() == 4 || abandon.load(); }) && inFlight.load() == 4) {
            sawAllFour.fetch_add(1);
        }
    });
    const bool allFour = waitUntil([&] { return sawAllFour.load() == 4; });
    abandon            = true;
    collectWithinBound(batch);

    EXPECT_TRUE(allFour);
}

// With nothing outstanding the workers wait on the condition variable, so they barely look.
TEST(WorkerPool, IdleWorkersPark) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 4};
    std::this_thread::sleep_for(50ms);  // each worker's first look, before it parks

    const std::uint64_t before = detail::WorkerPoolTestAccess::looks(pool);
    std::this_thread::sleep_for(100ms);
    const std::uint64_t after = detail::WorkerPoolTestAccess::looks(pool);

    EXPECT_LT(after - before, 16u);
}

// ── A job that throws ─────────────────────────────────────────────────────────────────────────────

// Jobs 3 and 7 throw different exceptions. Whichever throws first, collect rethrows job 3's, and a
// second collect on the same Batch does nothing. The job that throws second waits for the first one's
// flag and then a moment more, so the first exception has been recorded when the second arrives.
TEST(WorkerPool, TheLowestThrowingIndexIsRethrown) {
    for (const bool sevenFirst : {false, true}) {
        test::MockPlatform platform{0};
        WorkerPool         pool{platform, 2};
        std::atomic<bool>  threeThrown{false};
        std::atomic<bool>  sevenThrown{false};

        WorkerPool::Batch batch = pool.submit(10, [&](std::size_t i) {
            if (i == 3) {
                if (sevenFirst) {
                    holdUntil(sevenThrown);
                    std::this_thread::sleep_for(20ms);
                }
                threeThrown = true;
                throw std::runtime_error("job 3");
            }
            if (i == 7) {
                if (!sevenFirst) {
                    holdUntil(threeThrown);
                    std::this_thread::sleep_for(20ms);
                }
                sevenThrown = true;
                throw std::runtime_error("job 7");
            }
        });

        std::string thrown;
        try {
            collectWithinBound(batch);
        } catch (const std::runtime_error& error) {
            thrown = error.what();
        }
        EXPECT_EQ(thrown, "job 3") << (sevenFirst ? "job 7 threw first" : "job 3 threw first");
        EXPECT_NO_THROW(batch.collect());
    }
}

TEST(WorkerPool, EveryOtherJobRunsWhenOneThrows) {
    for (const std::size_t workers : {0u, 1u, 4u}) {
        test::MockPlatform            platform{0};
        WorkerPool                    pool{platform, workers};
        std::vector<std::atomic<int>> runs(100);

        WorkerPool::Batch batch = pool.submit(runs.size(), [&runs](std::size_t i) {
            runs[i].fetch_add(1);
            if (i == 10 || i == 50) {
                throw std::runtime_error("job " + std::to_string(i));
            }
        });
        EXPECT_THROW(collectWithinBound(batch), std::runtime_error);
        for (std::size_t i = 0; i < runs.size(); ++i) {
            ASSERT_EQ(runs[i].load(), 1) << "index " << i << " at " << workers << " workers";
        }
    }
}

// ── Several batches ───────────────────────────────────────────────────────────────────────────────

TEST(WorkerPool, TwoBatchesCollectInEitherOrder) {
    for (const bool firstThenSecond : {true, false}) {
        test::MockPlatform            platform{0};
        WorkerPool                    pool{platform, 2};
        std::vector<std::atomic<int>> first(1000);
        std::vector<std::atomic<int>> second(1000);

        WorkerPool::Batch a = pool.submit(first.size(), [&first](std::size_t i) { first[i].fetch_add(1); });
        WorkerPool::Batch b = pool.submit(second.size(), [&second](std::size_t i) { second[i].fetch_add(1); });
        if (firstThenSecond) {
            collectWithinBound(a);
            collectWithinBound(b);
        } else {
            collectWithinBound(b);
            collectWithinBound(a);
        }
        for (std::size_t i = 0; i < first.size(); ++i) {
            ASSERT_EQ(first[i].load(), 1) << "first batch, index " << i;
            ASSERT_EQ(second[i].load(), 1) << "second batch, index " << i;
        }
    }
}

// ── Shutdown ──────────────────────────────────────────────────────────────────────────────────────

TEST(WorkerPool, DestroyingThePoolFinishesItsBatches) {
    for (const std::size_t workers : {0u, 2u}) {
        test::MockPlatform            platform{0};
        std::vector<std::atomic<int>> runs(1000);
        WorkerPool::Batch             batch;
        withinBound("the pool's destructor", [&] {
            WorkerPool pool{platform, workers};
            batch = pool.submit(runs.size(), [&runs](std::size_t i) { runs[i].fetch_add(1); });
        });
        for (std::size_t i = 0; i < runs.size(); ++i) {
            ASSERT_EQ(runs[i].load(), 1) << "index " << i << " at " << workers << " workers";
        }
        batch.collect();
    }
}

TEST(WorkerPool, ABatchOutlivesItsPool) {
    test::MockPlatform       platform{0};
    std::atomic<std::size_t> ran{0};
    WorkerPool::Batch        quiet;
    WorkerPool::Batch        throwing;
    {
        WorkerPool pool{platform, 2};
        quiet    = pool.submit(100, [&ran](std::size_t) { ran.fetch_add(1); });
        throwing = pool.submit(4, [](std::size_t i) {
            if (i == 2) {
                throw std::runtime_error("job 2");
            }
        });
    }

    EXPECT_TRUE(quiet.done());
    EXPECT_TRUE(throwing.done());
    EXPECT_NO_THROW(quiet.collect());
    EXPECT_EQ(ran.load(), 100u);
    EXPECT_THROW(throwing.collect(), std::runtime_error);
}

TEST(WorkerPool, ABatchLeftUncollectedFinishesInItsDestructor) {
    test::MockPlatform            platform{0};
    WorkerPool                    pool{platform, 0};
    std::vector<std::atomic<int>> runs(100);

    {
        const WorkerPool::Batch batch =
            pool.submit(runs.size(), [&runs](std::size_t i) { runs[i].fetch_add(1); });
    }
    for (std::size_t i = 0; i < runs.size(); ++i) {
        ASSERT_EQ(runs[i].load(), 1) << "index " << i;
    }

    EXPECT_NO_THROW({
        const WorkerPool::Batch batch = pool.submit(10, [](std::size_t i) {
            if (i == 5) {
                throw std::runtime_error("job 5");
            }
        });
    });
}

TEST(WorkerPool, AMovedBatchLeavesAnEmptyOne) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 0};

    std::atomic<std::size_t> ran{0};
    WorkerPool::Batch        source = pool.submit(10, [&ran](std::size_t) { ran.fetch_add(1); });
    WorkerPool::Batch        target = std::move(source);
    EXPECT_TRUE(source.done());  // NOLINT(bugprone-use-after-move) — the moved-from state is the claim
    EXPECT_NO_THROW(source.collect());
    EXPECT_EQ(ran.load(), 0u);
    target.collect();
    EXPECT_EQ(ran.load(), 10u);

    std::vector<std::atomic<int>> first(10);
    std::vector<std::atomic<int>> second(10);
    WorkerPool::Batch held = pool.submit(first.size(), [&first](std::size_t i) { first[i].fetch_add(1); });
    held = pool.submit(second.size(), [&second](std::size_t i) { second[i].fetch_add(1); });
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].load(), 1) << "the batch assigned over, index " << i;
        EXPECT_EQ(second[i].load(), 0) << "the batch assigned, index " << i;
    }
    held.collect();
    for (std::size_t i = 0; i < second.size(); ++i) {
        EXPECT_EQ(second[i].load(), 1) << "index " << i;
    }
}

// ── Inside a job ──────────────────────────────────────────────────────────────────────────────────

TEST(WorkerPool, AJobThatSubmitsIsReported) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 0};
    WorkerPool::Batch  outer = pool.submit(1, [&pool](std::size_t) {
        const WorkerPool::Batch inner = pool.submit(1, [](std::size_t) {});
    });

    std::string reported;
    try {
        collectWithinBound(outer);
    } catch (const std::logic_error& error) {
        reported = error.what();
    }
    EXPECT_NE(reported.find("WorkerPool::submit"), std::string::npos) << reported;
}

TEST(WorkerPool, AJobThatCollectsIsReported) {
    test::MockPlatform platform{0};
    WorkerPool         pool{platform, 0};
    WorkerPool::Batch  other;
    WorkerPool::Batch  outer = pool.submit(1, [&other](std::size_t) { other.collect(); });

    std::string reported;
    try {
        collectWithinBound(outer);
    } catch (const std::logic_error& error) {
        reported = error.what();
    }
    EXPECT_NE(reported.find("WorkerPool::Batch::collect"), std::string::npos) << reported;
}

// ── The count ─────────────────────────────────────────────────────────────────────────────────────

TEST(WorkerPool, TheCountComesFromThePlatformsCores) {
    const CountInputs inputs;
    const auto        countAt = [](std::size_t cores) {
        test::MockPlatform platform{0};
        platform.setCoreCount(cores);
        const WorkerPool pool{platform};
        return pool.workers();
    };

    EXPECT_EQ(countAt(0), 0u);
    EXPECT_EQ(countAt(1), 0u);
    EXPECT_EQ(countAt(2), 1u);
    EXPECT_EQ(countAt(4), 3u);
    EXPECT_EQ(countAt(8), 7u);

    vm::machineThreads = 1;
    EXPECT_EQ(countAt(4), 2u);
    EXPECT_EQ(countAt(2), 1u);

    vm::machineThreads = 5;
    EXPECT_EQ(countAt(4), 1u);
}

TEST(WorkerPool, EveryWorkerStartsThroughThePlatform) {
    const CountInputs inputs;

    test::MockPlatform four{0};
    four.setCoreCount(4);
    {
        const WorkerPool pool{four};
        EXPECT_EQ(pool.workers(), 3u);
        EXPECT_EQ(four.threadStarts(), 3);
    }

    test::MockPlatform none{0};
    none.setCoreCount(4);
    {
        const WorkerPool pool{none, 0};
        EXPECT_EQ(pool.workers(), 0u);
        EXPECT_EQ(none.threadStarts(), 0);
    }
}

TEST(WorkerPool, ThePoolTakesItsCountFromTheConfig) {
    const CountInputs  inputs;
    test::MockPlatform platform{0};
    platform.setCoreCount(8);

    EngineConfig config = CountInputs::identified();
    config.workers      = 0;
    EngineConfig::setActive(config);
    EXPECT_EQ(WorkerPool{platform}.workers(), 0u);

    config.workers = 3;
    EngineConfig::setActive(config);
    EXPECT_EQ(WorkerPool{platform}.workers(), 3u);
    EXPECT_EQ((WorkerPool{platform, 5}.workers()), 5u);
}

TEST(WorkerPool, TheDefaultConfigLeavesTheCountToThePlatform) {
    const CountInputs inputs;
    EngineConfig::setActive(CountInputs::identified());
    EXPECT_FALSE(WorkerPool::defaultWorkers.has_value());

    test::MockPlatform platform{0};
    platform.setCoreCount(4);
    EXPECT_EQ(WorkerPool{platform}.workers(), 3u);
}

}  // namespace
}  // namespace retropp
