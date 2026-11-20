#pragma once

#include <cstdint>
#include <mutex>

#include "retropp/worker_pool.h"

namespace retropp::detail {

// Test-only reach into a WorkerPool's shared state, for the cases that pin how the pool synchronizes.
// INTERNAL — not part of include/retropp/.
class WorkerPoolTestAccess {
public:
    // The pool's mutex, held for as long as the returned lock lives. A path that takes no lock still
    // completes while it is held.
    [[nodiscard]] static std::unique_lock<std::mutex> holdLock(WorkerPool& pool);

    // How many times a worker has looked for a batch to run — once each time it evaluates its wait.
    [[nodiscard]] static std::uint64_t looks(const WorkerPool& pool);
};

}  // namespace retropp::detail
