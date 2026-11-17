#include "mge/transform2d.h"
#include "mge/jobs.h"
#include <algorithm>
#include <atomic>

namespace mge::scene2d {
using ecs::Entity;
using ecs::World;

void TransformSystem2D::attach(World& w, Entity e, vec2 pos, float rot, vec2 scale) {
    Transform2D t;
    t.position = pos; t.rotation = rot; t.scale = scale; t.dirty = 1;
    w.add<Transform2D>(e, t);
    w.add<WorldTransform2D>(e);
}

Entity TransformSystem2D::parentOf(World& w, Entity e) {
    Parent* p = w.get<Parent>(e);
    return p ? p->entity : Entity{};
}

bool TransformSystem2D::setParent(World& w, Entity child, Entity parent) {
    if (!parent.valid() || !w.alive(parent)) {
        if (w.has<Parent>(child)) { w.remove<Parent>(child); hierarchyDirty_ = true; }
        if (auto* t = w.get<Transform2D>(child)) t->dirty = 1;
        return true;
    }
    // Reject cycles: parent must not be child or one of child's descendants.
    for (Entity p = parent; p.valid(); p = parentOf(w, p))
        if (p == child) return false;
    if (Parent* existing = w.get<Parent>(child)) existing->entity = parent;
    else w.add<Parent>(child, {parent});
    if (auto* t = w.get<Transform2D>(child)) t->dirty = 1;
    hierarchyDirty_ = true;
    return true;
}

void TransformSystem2D::rebuildOrder(World& w) {
    struct Item { uint32_t depth; Entity e; };
    std::vector<Item> items;
    items.reserve(w.count<Parent>());
    w.each<Parent>([&](Entity e, Parent&) {
        uint32_t depth = 0;
        for (Entity p = e; ; ++depth) {
            Parent* pp = w.get<Parent>(p);
            if (!pp || !w.alive(pp->entity)) break;
            p = pp->entity;
        }
        items.push_back({depth, e});
    });
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.depth < y.depth; });
    ordered_.clear();
    for (auto& it : items) ordered_.push_back(it.e);
    hierarchyDirty_ = false;
}

void TransformSystem2D::update(World& w) {
    ++frame_;
    const uint32_t frame = frame_;

    // Detach children whose parent died (structural change, so collected first).
    orphans_.clear();
    w.each<Parent>([&](Entity e, Parent& p) { if (!w.alive(p.entity)) orphans_.push_back(e); });
    for (Entity e : orphans_) setParent(w, e, Entity{});
    if (hierarchyDirty_ || !orphans_.empty()) rebuildOrder(w);
    // Entities created/destroyed since last frame can change the child set without setParent.
    if (ordered_.size() != w.count<Parent>()) rebuildOrder(w);

    std::atomic<uint32_t> recomputed{0};

    // Roots: archetypes with a transform but no Parent component.
    w.eachChunk<Transform2D, WorldTransform2D>([&](uint32_t n, const Entity* ents, Transform2D* t, WorldTransform2D* wt) {
        if (w.has<Parent>(ents[0])) return;   // whole archetype shares the component set
        auto body = [&](uint32_t b, uint32_t e) {
            uint32_t local = 0;
            for (uint32_t i = b; i < e; ++i) {
                if (!t[i].dirty) continue;
                wt[i].m = t[i].local();
                wt[i].changedFrame = frame;
                t[i].dirty = 0;
                ++local;
            }
            recomputed.fetch_add(local, std::memory_order_relaxed);
        };
        if (n >= parallelThreshold && jobs::workerCount() > 0) jobs::parallelFor(n, 1024, body);
        else body(0, n);
    });

    // Children, parents first. Recompute if self dirty or the parent changed this frame.
    uint32_t childCount = 0;
    for (Entity e : ordered_) {
        Transform2D* t = w.get<Transform2D>(e);
        WorldTransform2D* wt = w.get<WorldTransform2D>(e);
        if (!t || !wt) continue;
        Entity pe = w.get<Parent>(e)->entity;
        WorldTransform2D* pw = w.get<WorldTransform2D>(pe);
        bool parentChanged = pw && pw->changedFrame == frame;
        if (!t->dirty && !parentChanged) continue;
        wt->m = pw ? pw->m * t->local() : t->local();
        wt->changedFrame = frame;
        t->dirty = 0;
        ++childCount;
    }
    recomputed_ = recomputed.load() + childCount;
}

}  // namespace mge::scene2d
