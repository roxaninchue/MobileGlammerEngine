#include "mge/jobs.h"
#include "mge/assert.h"
#include "mge/log.h"
#include "mge/math.h"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace mge::jobs {
namespace {

struct Job {
    JobFn fn;
    void* data;
    Counter* counter;
};

// Dmitry Vyukov's bounded MPMC queue.
class MpmcQueue {
public:
    void init(uint32_t capacity) {
        MGE_VERIFY((capacity & (capacity - 1)) == 0, "queue capacity must be a power of two");
        cells_ = std::vector<Cell>(capacity);
        mask_ = capacity - 1;
        for (uint32_t i = 0; i < capacity; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
        head_.store(0); tail_.store(0);
    }
    bool push(const Job& j) {
        uint32_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            uint32_t seq = c.seq.load(std::memory_order_acquire);
            int32_t diff = int32_t(seq - pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.job = j;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;   // full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }
    bool pop(Job& out) {
        uint32_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & mask_];
            uint32_t seq = c.seq.load(std::memory_order_acquire);
            int32_t diff = int32_t(seq - (pos + 1));
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = c.job;
                    c.seq.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;   // empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
private:
    struct Cell { std::atomic<uint32_t> seq{0}; Job job{}; };
    std::vector<Cell> cells_;
    uint32_t mask_ = 0;
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
};

struct System {
    MpmcQueue queue;
    std::vector<std::thread> workers;
    std::mutex sleepMutex;
    std::condition_variable sleepCv;
    std::atomic<uint32_t> pending{0};   // jobs queued but not yet popped
    std::atomic<bool> running{false};
};
System* g = nullptr;

void runJob(const Job& j) {
    j.fn(j.data);
    if (j.counter) j.counter->value.fetch_sub(1, std::memory_order_acq_rel);
}

bool tryRunOne() {
    Job j;
    if (!g->queue.pop(j)) return false;
    g->pending.fetch_sub(1, std::memory_order_relaxed);
    runJob(j);
    return true;
}

void workerMain(int index) {
    (void)index;
    while (g->running.load(std::memory_order_acquire)) {
        if (tryRunOne()) continue;
        std::unique_lock lock(g->sleepMutex);
        g->sleepCv.wait(lock, [] {
            return !g->running.load(std::memory_order_acquire) || g->pending.load(std::memory_order_acquire) > 0;
        });
    }
}

// On big.LITTLE, more workers than big cores just pushes work onto slow cores and heats the phone.
int detectWorkerCount() {
    int hw = int(std::thread::hardware_concurrency());
    if (hw <= 1) return 1;
    int workers = hw - 1;   // leave the main/render thread its core
#if defined(__ANDROID__) || defined(__linux__)
    long maxFreq = 0;
    std::vector<long> freqs;
    for (int i = 0; i < hw; ++i) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        long f = 0;
        if (FILE* fp = std::fopen(path, "r")) { if (std::fscanf(fp, "%ld", &f) != 1) f = 0; std::fclose(fp); }
        freqs.push_back(f);
        if (f > maxFreq) maxFreq = f;
    }
    if (maxFreq > 0) {
        int fast = 0;   // "fast" = at least 70% of the top clock (mid + prime cores)
        for (long f : freqs) if (f * 10 >= maxFreq * 7) ++fast;
        if (fast > 1) workers = min(workers, fast);
    }
#endif
    return max(1, workers);
}

}  // namespace

bool init(const Config& cfg) {
    if (g) return true;
    g = new System();
    g->queue.init(cfg.queueCapacity);
    int n = cfg.workerCount > 0 ? cfg.workerCount : detectWorkerCount();
    g->running.store(true, std::memory_order_release);
    for (int i = 0; i < n; ++i) g->workers.emplace_back(workerMain, i);
    MGE_LOGI("Job system: %d workers", n);
    return true;
}

void shutdown() {
    if (!g) return;
    {
        std::lock_guard lock(g->sleepMutex);
        g->running.store(false, std::memory_order_release);
    }
    g->sleepCv.notify_all();
    for (auto& t : g->workers) t.join();
    while (tryRunOne()) {}   // drain anything left so counters reach zero
    delete g;
    g = nullptr;
}

int workerCount() { return g ? int(g->workers.size()) : 0; }

bool submit(JobFn fn, void* data, Counter* counter) {
    MGE_ASSERT(g, "jobs::init not called");
    if (counter) counter->value.fetch_add(1, std::memory_order_relaxed);
    Job j{fn, data, counter};
    if (!g->queue.push(j)) {
        runJob(j);   // queue full: run inline rather than drop
        return false;
    }
    {
        // Lock pairs with the worker's predicate check so a wakeup can't be missed.
        std::lock_guard lock(g->sleepMutex);
        g->pending.fetch_add(1, std::memory_order_release);
    }
    g->sleepCv.notify_one();
    return true;
}

void wait(Counter& counter) {
    while (!counter.done()) {
        if (!tryRunOne()) std::this_thread::yield();
    }
}

void parallelFor(uint32_t count, uint32_t batchSize, const std::function<void(uint32_t, uint32_t)>& body) {
    if (count == 0) return;
    if (batchSize == 0) batchSize = 1;
    uint32_t batches = (count + batchSize - 1) / batchSize;
    if (batches == 1 || !g) { body(0, count); return; }

    struct Batch { const std::function<void(uint32_t, uint32_t)>* body; uint32_t begin, end; };
    // Small batch counts stay on the stack; this keeps parallelFor allocation-free in the common case.
    constexpr uint32_t kStack = 256;
    Batch stackBatches[kStack];
    std::vector<Batch> heapBatches;
    Batch* bs = stackBatches;
    if (batches > kStack) { heapBatches.resize(batches); bs = heapBatches.data(); }

    Counter counter;
    for (uint32_t b = 1; b < batches; ++b) {   // batch 0 runs on the caller
        bs[b] = {&body, b * batchSize, min(count, (b + 1) * batchSize)};
        submit([](void* d) { auto* x = static_cast<Batch*>(d); (*x->body)(x->begin, x->end); }, &bs[b], &counter);
    }
    body(0, min(count, batchSize));
    wait(counter);
}

}  // namespace mge::jobs
