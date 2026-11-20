// Hands a pool a lambda that captures by reference. This builds — the control that proves the outcome
// harness compiles a well-formed job, so a negative that fails to build for another reason is caught.
#include <array>
#include <cstddef>

#include "retropp/worker_pool.h"

void submitAJobCapturingReferences(retropp::WorkerPool& pool) {
    std::array<std::size_t, 16> values{};
    const std::size_t           scale = 3;
    retropp::WorkerPool::Batch  batch =
        pool.submit(values.size(), [&values, &scale](std::size_t i) { values[i] = i * scale; });
    batch.collect();
}
