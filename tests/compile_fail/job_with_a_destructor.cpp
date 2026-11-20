// Hands a pool a lambda that captures a string by value, so the job has a destructor to run. This does
// not build — the outcome tests/compile_outcome.cmake asserts.
#include <cstddef>
#include <string>

#include "retropp/worker_pool.h"

void submitAJobWithADestructor(retropp::WorkerPool& pool, std::size_t* out) {
    const std::string name = "worker";
    retropp::WorkerPool::Batch batch = pool.submit(1, [name, out](std::size_t i) { out[i] = name.size(); });
    batch.collect();
}
