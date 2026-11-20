// Hands a pool a lambda that captures a table by value, past the room a batch has for its job. This does
// not build — the outcome tests/compile_outcome.cmake asserts.
#include <array>
#include <cstddef>
#include <cstdint>

#include "retropp/worker_pool.h"

void submitAJobPastTheStorage(retropp::WorkerPool& pool, std::uint64_t* out) {
    const std::array<std::uint64_t, 9> table{};
    retropp::WorkerPool::Batch batch =
        pool.submit(table.size(), [table, out](std::size_t i) { out[i] = table[i]; });
    batch.collect();
}
