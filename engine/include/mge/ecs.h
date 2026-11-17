#pragma once
// MGE ECS: archetype storage, structure-of-arrays columns, generational entity ids.
//  - Components are plain data (trivially copyable), so moving between archetypes is memcpy.
//  - Up to 64 component types per world in M1 (one bit each in the archetype mask).
//  - Queries walk matching archetypes column by column: linear, cache-friendly, no per-entity lookups.
//  - No structural changes (create/destroy/add/remove) while a query runs; asserted in debug.
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "mge/assert.h"
#include "mge/memory.h"

namespace mge::ecs {

struct Entity {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    bool valid() const { return index != UINT32_MAX; }
    bool operator==(const Entity& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

using ComponentId = uint32_t;
constexpr ComponentId kMaxComponents = 64;

namespace detail {
struct ComponentInfo { uint32_t size; uint32_t align; const char* name; };
ComponentId registerComponent(uint32_t size, uint32_t align, const char* name);
const ComponentInfo& componentInfo(ComponentId id);
}  // namespace detail

// Stable per-process id for a component type (assigned on first use).
template <class T> ComponentId componentId() {
    static_assert(std::is_trivially_copyable_v<T>, "ECS components must be plain data");
    static const ComponentId id = detail::registerComponent(sizeof(T), alignof(T), __PRETTY_FUNCTION__);
    return id;
}
template <class T> uint64_t componentMask() { return uint64_t(1) << componentId<T>(); }

struct Archetype {
    uint64_t mask = 0;
    uint32_t count = 0, capacity = 0;
    Entity* entities = nullptr;
    void* columns[kMaxComponents] = {};   // indexed by ComponentId; null when not in mask

    bool has(ComponentId c) const { return (mask >> c) & 1; }
    template <class T> T* column() { return static_cast<T*>(columns[componentId<T>()]); }
};

class World {
public:
    World() = default;
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Entity create();
    void destroy(Entity e);
    bool alive(Entity e) const {
        return e.index < records_.size() && records_[e.index].generation == e.generation && records_[e.index].archetype;
    }
    uint32_t aliveCount() const { return alive_; }

    template <class T> T& add(Entity e, const T& value = T{}) {
        ComponentId c = componentId<T>();
        Record& r = record(e);
        if (!archetypes_[r.archetype - 1]->has(c)) moveTo(e, archetypes_[r.archetype - 1]->mask | (uint64_t(1) << c));
        Archetype* a = archetypes_[r.archetype - 1];
        T* slot = static_cast<T*>(a->columns[c]) + r.row;
        std::memcpy(static_cast<void*>(slot), &value, sizeof(T));
        return *slot;
    }
    template <class T> void remove(Entity e) {
        ComponentId c = componentId<T>();
        Record& r = record(e);
        if (archetypes_[r.archetype - 1]->has(c)) moveTo(e, archetypes_[r.archetype - 1]->mask & ~(uint64_t(1) << c));
    }
    template <class T> T* get(Entity e) {
        if (!alive(e)) return nullptr;
        const Record& r = records_[e.index];
        Archetype* a = archetypes_[r.archetype - 1];
        ComponentId c = componentId<T>();
        return a->has(c) ? static_cast<T*>(a->columns[c]) + r.row : nullptr;
    }
    template <class T> bool has(Entity e) { return get<T>(e) != nullptr; }

    // Type-erased access for serialization/editor. addRaw zero-fills a newly added component.
    void* getRaw(Entity e, ComponentId c) {
        if (!alive(e)) return nullptr;
        const Record& r = records_[e.index];
        Archetype* a = archetypes_[r.archetype - 1];
        return a->has(c) ? static_cast<uint8_t*>(a->columns[c]) + size_t(detail::componentInfo(c).size) * r.row : nullptr;
    }
    void* addRaw(Entity e, ComponentId c) {
        Record& r = record(e);
        if (!archetypes_[r.archetype - 1]->has(c)) moveTo(e, archetypes_[r.archetype - 1]->mask | (uint64_t(1) << c));
        Archetype* a = archetypes_[r.archetype - 1];
        return static_cast<uint8_t*>(a->columns[c]) + size_t(detail::componentInfo(c).size) * r.row;
    }
    template <class Fn> void eachEntity(Fn&& fn) {
        ++iterating_;
        for (Archetype* a : archetypes_)
            for (uint32_t i = 0; i < a->count; ++i) fn(a->entities[i]);
        --iterating_;
    }

    // fn(Entity, Ts&...) for every entity that has all of Ts.
    template <class... Ts, class Fn> void each(Fn&& fn) {
        const uint64_t need = (componentMask<Ts>() | ... | 0);
        ++iterating_;
        for (Archetype* a : archetypes_) {
            if ((a->mask & need) != need || a->count == 0) continue;
            auto cols = std::make_tuple(a->template column<Ts>()...);
            for (uint32_t i = 0; i < a->count; ++i)
                fn(a->entities[i], std::get<Ts*>(cols)[i]...);
        }
        --iterating_;
    }
    // Chunk form for the job system / SIMD: fn(count, const Entity*, Ts*...) per archetype.
    template <class... Ts, class Fn> void eachChunk(Fn&& fn) {
        const uint64_t need = (componentMask<Ts>() | ... | 0);
        ++iterating_;
        for (Archetype* a : archetypes_)
            if ((a->mask & need) == need && a->count) fn(a->count, a->entities, a->template column<Ts>()...);
        --iterating_;
    }
    template <class... Ts> uint32_t count() {
        const uint64_t need = (componentMask<Ts>() | ... | 0);
        uint32_t n = 0;
        for (Archetype* a : archetypes_) if ((a->mask & need) == need) n += a->count;
        return n;
    }

    size_t archetypeCount() const { return archetypes_.size(); }
    size_t memoryBytes() const;

private:
    struct Record { uint32_t generation = 0; uint32_t archetype = 0; uint32_t row = 0; };  // archetype: 1-based, 0 = dead

    Record& record(Entity e) {
        MGE_ASSERT(alive(e), "stale or invalid entity");
        MGE_ASSERT(iterating_ == 0, "structural change during a query");
        return records_[e.index];
    }
    uint32_t archetypeFor(uint64_t mask);
    void grow(Archetype& a);
    uint32_t pushRow(Archetype& a, Entity e);
    void removeRow(Archetype& a, uint32_t row);
    void moveTo(Entity e, uint64_t newMask);

    std::vector<Record> records_;
    std::vector<uint32_t> freeList_;
    std::vector<Archetype*> archetypes_;
    std::unordered_map<uint64_t, uint32_t> byMask_;
    uint32_t alive_ = 0;
    int iterating_ = 0;
};

}  // namespace mge::ecs
