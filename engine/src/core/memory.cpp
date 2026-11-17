#include "mge/memory.h"
#include <atomic>
#include <cstdlib>

namespace mge {
namespace {
std::atomic<size_t> g_liveBytes{0}, g_liveAllocs{0}, g_peakBytes{0};

struct Header { size_t size; size_t offset; };   // stored just before the user pointer
}

void* memAlloc(size_t size, size_t align) {
    if (align < alignof(Header)) align = alignof(Header);
    size_t total = size + align + sizeof(Header);
    auto* raw = static_cast<uint8_t*>(std::malloc(total));
    if (!raw) return nullptr;
    uintptr_t user = alignUp(reinterpret_cast<uintptr_t>(raw) + sizeof(Header), align);
    auto* h = reinterpret_cast<Header*>(user) - 1;
    h->size = size;
    h->offset = user - reinterpret_cast<uintptr_t>(raw);
    size_t live = g_liveBytes.fetch_add(size, std::memory_order_relaxed) + size;
    g_liveAllocs.fetch_add(1, std::memory_order_relaxed);
    size_t peak = g_peakBytes.load(std::memory_order_relaxed);
    while (live > peak && !g_peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
    return reinterpret_cast<void*>(user);
}

void memFree(void* p) {
    if (!p) return;
    auto* h = static_cast<Header*>(p) - 1;
    g_liveBytes.fetch_sub(h->size, std::memory_order_relaxed);
    g_liveAllocs.fetch_sub(1, std::memory_order_relaxed);
    std::free(static_cast<uint8_t*>(p) - h->offset);
}

MemStats memStats() {
    return {g_liveBytes.load(), g_liveAllocs.load(), g_peakBytes.load()};
}

void Arena::init(size_t capacity) {
    release();
    base_ = static_cast<uint8_t*>(memAlloc(capacity, 64));
    MGE_VERIFY(base_, "arena allocation failed");
    cap_ = capacity; off_ = peak_ = 0; owns_ = true;
}

void Arena::initExternal(void* buffer, size_t capacity) {
    release();
    base_ = static_cast<uint8_t*>(buffer);
    cap_ = capacity; off_ = peak_ = 0; owns_ = false;
}

void Arena::release() {
    if (owns_) memFree(base_);
    base_ = nullptr; cap_ = off_ = peak_ = 0; owns_ = false;
}

}  // namespace mge
