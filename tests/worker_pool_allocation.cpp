// A warm WorkerPool hands a batch over and collects it without allocating on the calling thread. This
// program replaces the global operator new and operator delete with ones that count each thread's
// allocations, so it is an executable of its own: the replacement would count every other case's
// allocations too. It exits 0 when the counted rounds allocate nothing and 1 otherwise.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "mock_platform.h"
#include "retropp/worker_pool.h"

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

// Every allocation the calling thread makes through operator new, in any form.
thread_local std::size_t allocations = 0;

void* allocate(std::size_t size) {
    ++allocations;
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc{};
}

void* allocateAligned(std::size_t size, std::align_val_t align) {
    ++allocations;
    const auto alignment = static_cast<std::size_t>(align);
    void*      block     = nullptr;
#if defined(_WIN32)
    block = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
    if (posix_memalign(&block, alignment < sizeof(void*) ? sizeof(void*) : alignment, size == 0 ? 1 : size) != 0) {
        block = nullptr;
    }
#endif
    if (block == nullptr) {
        throw std::bad_alloc{};
    }
    return block;
}

void releaseAligned(void* block) noexcept {
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { std::free(block); }

void* operator new(std::size_t size, std::align_val_t align) { return allocateAligned(size, align); }
void* operator new[](std::size_t size, std::align_val_t align) { return allocateAligned(size, align); }
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(size, align);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(size, align);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block, std::align_val_t) noexcept { releaseAligned(block); }
void operator delete[](void* block, std::align_val_t) noexcept { releaseAligned(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept { releaseAligned(block); }
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept { releaseAligned(block); }
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(block); }
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(block); }

int main() {
    retropp::test::MockPlatform platform{0};
    retropp::WorkerPool         pool{platform, 2};
    std::array<std::uint32_t, 64> out{};

    // One batch handed over and collected.
    const auto oneRound = [&] {
        retropp::WorkerPool::Batch batch =
            pool.submit(out.size(), [&out](std::size_t i) { out[i] = static_cast<std::uint32_t>(i * 3); });
        batch.collect();
    };
    // Four batches held at once, each writing its own row, then collected.
    std::array<std::array<std::uint32_t, 64>, 4> rows{};
    const auto fourAtOnce = [&] {
        const auto job = [&rows](std::size_t row) {
            return [&rows, row](std::size_t i) { rows[row][i] = static_cast<std::uint32_t>(row + i); };
        };
        retropp::WorkerPool::Batch a = pool.submit(rows[0].size(), job(0));
        retropp::WorkerPool::Batch b = pool.submit(rows[1].size(), job(1));
        retropp::WorkerPool::Batch c = pool.submit(rows[2].size(), job(2));
        retropp::WorkerPool::Batch d = pool.submit(rows[3].size(), job(3));
        a.collect();
        b.collect();
        c.collect();
        d.collect();
    };

    for (int round = 0; round < 16; ++round) {
        oneRound();
    }
    const std::size_t beforeOne = allocations;
    for (int round = 0; round < 1000; ++round) {
        oneRound();
    }
    const std::size_t oneRoundAllocations = allocations - beforeOne;

    for (int round = 0; round < 16; ++round) {
        fourAtOnce();
    }
    const std::size_t beforeFour = allocations;
    for (int round = 0; round < 1000; ++round) {
        fourAtOnce();
    }
    const std::size_t fourAtOnceAllocations = allocations - beforeFour;

    std::printf("1000 rounds of one batch: %zu allocations on the calling thread\n", oneRoundAllocations);
    std::printf("1000 rounds of four batches held at once: %zu allocations on the calling thread\n",
                fourAtOnceAllocations);
    return oneRoundAllocations == 0 && fourAtOnceAllocations == 0 ? 0 : 1;
}
