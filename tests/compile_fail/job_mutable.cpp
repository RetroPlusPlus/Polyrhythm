// Hands a pool a lambda that changes its own capture. A job is called through a const reference on
// several threads at once, so this does not build — the outcome tests/compile_outcome.cmake asserts.
#include <cstddef>

#include "retropp/worker_pool.h"

void submitAMutableJob(retropp::WorkerPool& pool) {
    int calls = 0;
    retropp::WorkerPool::Batch batch = pool.submit(4, [calls](std::size_t) mutable { ++calls; });
    batch.collect();
}
