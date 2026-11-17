#include "mge/ecs.h"

namespace mge::ecs {
namespace detail {

static ComponentInfo gInfos[kMaxComponents];
static ComponentId gCount = 0;

ComponentId registerComponent(uint32_t size, uint32_t align, const char* name) {
    MGE_VERIFY(gCount < kMaxComponents, "too many ECS component types (M1 limit is 64)");
    gInfos[gCount] = {size, align, name};
    return gCount++;
}
const ComponentInfo& componentInfo(ComponentId id) { return gInfos[id]; }

}  // namespace detail

World::~World() {
    for (Archetype* a : archetypes_) {
        for (ComponentId c = 0; c < kMaxComponents; ++c) if (a->columns[c]) memFree(a->columns[c]);
        memFree(a->entities);
        delete a;
    }
}

Entity World::create() {
    uint32_t idx;
    if (!freeList_.empty()) { idx = freeList_.back(); freeList_.pop_back(); }
    else { idx = uint32_t(records_.size()); records_.push_back({}); }
    MGE_ASSERT(iterating_ == 0, "structural change during a query");
    Entity e{idx, records_[idx].generation};
    uint32_t arch = archetypeFor(0);
    records_[idx].archetype = arch;
    records_[idx].row = pushRow(*archetypes_[arch - 1], e);
    ++alive_;
    return e;
}

void World::destroy(Entity e) {
    if (!alive(e)) return;
    Record& r = record(e);
    removeRow(*archetypes_[r.archetype - 1], r.row);
    r.archetype = 0;
    ++r.generation;   // old handles go stale
    freeList_.push_back(e.index);
    --alive_;
}

uint32_t World::archetypeFor(uint64_t mask) {
    auto it = byMask_.find(mask);
    if (it != byMask_.end()) return it->second;
    auto* a = new Archetype();
    a->mask = mask;
    archetypes_.push_back(a);
    uint32_t id = uint32_t(archetypes_.size());
    byMask_.emplace(mask, id);
    return id;
}

void World::grow(Archetype& a) {
    uint32_t cap = a.capacity ? a.capacity * 2 : 64;
    auto* ents = static_cast<Entity*>(memAlloc(sizeof(Entity) * cap, alignof(Entity)));
    MGE_VERIFY(ents, "ECS out of memory");
    if (a.count) std::memcpy(ents, a.entities, sizeof(Entity) * a.count);
    memFree(a.entities);
    a.entities = ents;
    for (ComponentId c = 0; c < kMaxComponents; ++c) {
        if (!a.has(c)) continue;
        const auto& info = detail::componentInfo(c);
        void* col = memAlloc(size_t(info.size) * cap, info.align < 16 ? 16 : info.align);
        MGE_VERIFY(col, "ECS out of memory");
        if (a.count) std::memcpy(col, a.columns[c], size_t(info.size) * a.count);
        memFree(a.columns[c]);
        a.columns[c] = col;
    }
    a.capacity = cap;
}

uint32_t World::pushRow(Archetype& a, Entity e) {
    if (a.count == a.capacity) grow(a);
    uint32_t row = a.count++;
    a.entities[row] = e;
    for (ComponentId c = 0; c < kMaxComponents; ++c)   // zero-init so unset components are defined
        if (a.has(c)) { auto sz = detail::componentInfo(c).size; std::memset(static_cast<uint8_t*>(a.columns[c]) + size_t(sz) * row, 0, sz); }
    return row;
}

// Swap-remove: last row fills the hole, its record is patched.
void World::removeRow(Archetype& a, uint32_t row) {
    uint32_t last = a.count - 1;
    if (row != last) {
        a.entities[row] = a.entities[last];
        for (ComponentId c = 0; c < kMaxComponents; ++c) {
            if (!a.has(c)) continue;
            auto sz = detail::componentInfo(c).size;
            auto* base = static_cast<uint8_t*>(a.columns[c]);
            std::memcpy(base + size_t(sz) * row, base + size_t(sz) * last, sz);
        }
        records_[a.entities[row].index].row = row;
    }
    a.count = last;
}

void World::moveTo(Entity e, uint64_t newMask) {
    uint32_t dstId = archetypeFor(newMask);   // may grow archetypes_, so look pointers up after
    Record& r = records_[e.index];
    Archetype& src = *archetypes_[r.archetype - 1];
    Archetype& dst = *archetypes_[dstId - 1];
    uint32_t row = pushRow(dst, e);
    for (ComponentId c = 0; c < kMaxComponents; ++c) {
        if (!src.has(c) || !dst.has(c)) continue;
        auto sz = detail::componentInfo(c).size;
        std::memcpy(static_cast<uint8_t*>(dst.columns[c]) + size_t(sz) * row,
                    static_cast<uint8_t*>(src.columns[c]) + size_t(sz) * r.row, sz);
    }
    removeRow(src, r.row);
    r.archetype = dstId;
    r.row = row;
}

size_t World::memoryBytes() const {
    size_t b = records_.capacity() * sizeof(Record) + freeList_.capacity() * sizeof(uint32_t);
    for (const Archetype* a : archetypes_) {
        b += sizeof(Archetype) + size_t(a->capacity) * sizeof(Entity);
        for (ComponentId c = 0; c < kMaxComponents; ++c)
            if (a->has(c)) b += size_t(detail::componentInfo(c).size) * a->capacity;
    }
    return b;
}

}  // namespace mge::ecs
