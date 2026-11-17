#include "test_main.h"
#include "mge/memory.h"
using namespace mge;

TEST(arena_alloc_align_rewind) {
    Arena a(1 * KiB);
    void* p1 = a.alloc(3, 1);
    void* p2 = a.alloc(16, 64);
    CHECK(p1 && p2);
    CHECK(reinterpret_cast<uintptr_t>(p2) % 64 == 0);
    auto m = a.mark();
    a.alloc(100);
    a.rewind(m);
    CHECK(a.used() == m);
    {
        ArenaScope scope(a);
        a.alloc(200);
    }
    CHECK(a.used() == m);
    CHECK(a.alloc(4 * KiB) == nullptr);   // over capacity fails cleanly
    a.reset();
    CHECK(a.used() == 0 && a.peak() >= 280);
}

TEST(frame_allocator_recycles) {
    FrameAllocator<2> fa;
    fa.init(256);
    fa.beginFrame();
    int* x = fa.array<int>(10);
    x[0] = 42;
    fa.beginFrame();
    fa.array<int>(10);
    CHECK(x[0] == 42);   // previous frame's memory still intact while in flight
    fa.beginFrame();     // wraps back: that arena is reset
    CHECK(fa.current().used() == 0);
}

struct Thing { int a; float b; Thing(int x, float y) : a(x), b(y) {} };

TEST(pool_create_destroy) {
    Pool<Thing> pool(4);
    Thing* t[4];
    for (int i = 0; i < 4; ++i) t[i] = pool.create(i, 1.5f);
    CHECK(pool.create(9, 9.f) == nullptr);
    CHECK(pool.live() == 4 && t[2]->a == 2);
    pool.destroy(t[1]);
    Thing* again = pool.create(7, 0.f);
    CHECK(again == t[1] && again->a == 7);
    CHECK(pool.owns(t[3]));
}

TEST(mem_tracking) {
    MemStats before = memStats();
    void* p = memAlloc(1000, 32);
    CHECK(reinterpret_cast<uintptr_t>(p) % 32 == 0);
    CHECK(memStats().liveBytes == before.liveBytes + 1000);
    memFree(p);
    CHECK(memStats().liveBytes == before.liveBytes);
}
