#include "test_main.h"
#include "mge/jobs.h"
#include <atomic>
#include <vector>
using namespace mge;

TEST(jobs_submit_and_wait) {
    jobs::Config cfg;
    cfg.workerCount = 4;   // always exercise real contention, even on 1-core CI/sandboxes
    jobs::init(cfg);
    std::atomic<int> sum{0};
    jobs::Counter c;
    static std::atomic<int>* s;
    s = &sum;
    for (int i = 0; i < 1000; ++i) jobs::submit([](void*) { s->fetch_add(1); }, nullptr, &c);
    jobs::wait(c);
    CHECK(sum.load() == 1000);
    CHECK(c.done());
}

TEST(jobs_parallel_for) {
    std::vector<int> v(100000, 1);
    jobs::parallelFor(uint32_t(v.size()), 1024, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) v[i] *= 2;
    });
    long total = 0;
    for (int x : v) total += x;
    CHECK(total == 200000);
}

TEST(jobs_nested_wait) {
    // Jobs that themselves wait on child jobs must not deadlock.
    std::atomic<int> count{0};
    jobs::parallelFor(64, 1, [&](uint32_t, uint32_t) {
        jobs::parallelFor(16, 1, [&](uint32_t, uint32_t) { count.fetch_add(1); });
    });
    CHECK(count.load() == 64 * 16);
    jobs::shutdown();
}
