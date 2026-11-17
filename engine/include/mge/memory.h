#pragma once
// MGE allocators. Goal: zero heap allocations per frame.
//  - Arena: bump allocator over a fixed block, reset or rewind to a marker.
//  - FrameAllocator: N arenas (one per frame in flight); memory lives until the frame is recycled.
//  - Pool<T>: fixed-size object pool with an intrusive free list, O(1) alloc/free.
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include "mge/assert.h"

namespace mge {

constexpr size_t KiB = 1024, MiB = 1024 * KiB;
inline size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// Tracked raw allocation (counts bytes in debug so leaks show up).
void* memAlloc(size_t size, size_t align = alignof(std::max_align_t));
void memFree(void* p);
struct MemStats { size_t liveBytes; size_t liveAllocs; size_t peakBytes; };
MemStats memStats();

class Arena {
public:
    Arena() = default;
    explicit Arena(size_t capacity) { init(capacity); }
    ~Arena() { release(); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& o) noexcept { *this = std::move(o); }
    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            release();
            base_ = o.base_; cap_ = o.cap_; off_ = o.off_; peak_ = o.peak_; owns_ = o.owns_;
            o.base_ = nullptr; o.cap_ = o.off_ = o.peak_ = 0; o.owns_ = false;
        }
        return *this;
    }

    void init(size_t capacity);
    void initExternal(void* buffer, size_t capacity);   // arena over memory it does not own
    void release();

    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        size_t start = alignUp(reinterpret_cast<uintptr_t>(base_) + off_, align) - reinterpret_cast<uintptr_t>(base_);
        if (start + size > cap_) return nullptr;   // caller decides: grow, fall back, or fail
        off_ = start + size;
        if (off_ > peak_) peak_ = off_;
        return base_ + start;
    }
    template <class T, class... Args> T* make(Args&&... args) {
        void* p = alloc(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }
    // Uninitialized array; T must be trivially destructible (arena never runs destructors).
    template <class T> T* array(size_t n) { return static_cast<T*>(alloc(sizeof(T) * n, alignof(T))); }

    using Marker = size_t;
    Marker mark() const { return off_; }
    void rewind(Marker m) { MGE_ASSERT(m <= off_); off_ = m; }
    void reset() { off_ = 0; }

    size_t used() const { return off_; }
    size_t capacity() const { return cap_; }
    size_t peak() const { return peak_; }

private:
    uint8_t* base_ = nullptr;
    size_t cap_ = 0, off_ = 0, peak_ = 0;
    bool owns_ = false;
};

// Scope guard: everything allocated inside the scope is released at the end.
class ArenaScope {
public:
    explicit ArenaScope(Arena& a) : arena_(a), mark_(a.mark()) {}
    ~ArenaScope() { arena_.rewind(mark_); }
private:
    Arena& arena_;
    Arena::Marker mark_;
};

template <int FramesInFlight = 2>
class FrameAllocator {
public:
    void init(size_t perFrameBytes) { for (auto& a : arenas_) a.init(perFrameBytes); }
    // Call at frame start: recycles the arena of the frame that just left flight.
    void beginFrame() { index_ = (index_ + 1) % FramesInFlight; arenas_[index_].reset(); }
    Arena& current() { return arenas_[index_]; }
    void* alloc(size_t size, size_t align = alignof(std::max_align_t)) { return current().alloc(size, align); }
    template <class T> T* array(size_t n) { return current().template array<T>(n); }
private:
    Arena arenas_[FramesInFlight];
    int index_ = 0;
};

template <class T>
class Pool {
public:
    Pool() = default;
    explicit Pool(size_t capacity) { init(capacity); }
    ~Pool() { memFree(slots_); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    void init(size_t capacity) {
        MGE_ASSERT(!slots_);
        slots_ = static_cast<Slot*>(memAlloc(sizeof(Slot) * capacity, alignof(Slot)));
        MGE_VERIFY(slots_, "pool allocation failed");
        cap_ = capacity;
        free_ = nullptr;
        for (size_t i = capacity; i-- > 0;) { slots_[i].next = free_; free_ = &slots_[i]; }
    }
    template <class... Args> T* create(Args&&... args) {
        if (!free_) return nullptr;
        Slot* s = free_;
        free_ = s->next;
        ++live_;
        return new (s->storage) T(std::forward<Args>(args)...);
    }
    void destroy(T* obj) {
        if (!obj) return;
        MGE_ASSERT(owns(obj), "object not from this pool");
        obj->~T();
        Slot* s = reinterpret_cast<Slot*>(obj);
        s->next = free_;
        free_ = s;
        --live_;
    }
    bool owns(const T* p) const {
        auto a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(slots_);
        return a >= b && a < b + cap_ * sizeof(Slot) && (a - b) % sizeof(Slot) == 0;
    }
    size_t live() const { return live_; }
    size_t capacity() const { return cap_; }

private:
    union Slot {
        Slot* next;
        alignas(T) unsigned char storage[sizeof(T)];
    };
    Slot* slots_ = nullptr;
    Slot* free_ = nullptr;
    size_t cap_ = 0, live_ = 0;
};

}  // namespace mge
