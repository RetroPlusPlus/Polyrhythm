#pragma once

#include <atomic>
#include <cstddef>

namespace retropp::vm {

// The machines running on their own thread right now. A threaded VmRunner counts itself in before its
// thread is created and out as its loop leaves; an inline runner is never counted. A WorkerPool made
// without a count reads it once, at construction, and leaves a core for each.
inline std::atomic<std::size_t> machineThreads{0};

}  // namespace retropp::vm
