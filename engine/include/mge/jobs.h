#pragma once
// MGE job system: fixed worker pool, bounded lock-free MPMC queue, counter-based waits.
// Waiting threads help run jobs instead of blocking, so there is no deadlock on nested waits.
// Workers park on a condition variable when idle: zero CPU burned when there is no work.
#include <atomic>
#include <cstdint>
#include <functional>

namespace mge::jobs {

using JobFn = void (*)(void* data);

// Completion counter. Starts at 0; each job submitted with it adds 1, each finished job subtracts 1.
struct Counter {
    std::atomic<uint32_t> value{0};
    bool done() const { return value.load(std::memory_order_acquire) == 0; }
};

struct Config {
    int workerCount = -1;   // -1: auto (cores - 1, capped by big cores on big.LITTLE)
    uint32_t queueCapacity = 4096;   // power of two
};

bool init(const Config& cfg = {});
void shutdown();
int workerCount();

// Submit a job. counter may be null (fire and forget). Returns false only if the queue is full
// and the job had to run inline on the caller.
bool submit(JobFn fn, void* data, Counter* counter = nullptr);

// Block until counter reaches 0, running other jobs while waiting.
void wait(Counter& counter);

// Split [0, count) into batches and run body(begin, end) across workers. Blocks until done.
void parallelFor(uint32_t count, uint32_t batchSize, const std::function<void(uint32_t, uint32_t)>& body);

}  // namespace mge::jobs
