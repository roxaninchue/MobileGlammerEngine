#include "mge/transform3d.h"
#include <algorithm>

namespace mge::scene3d {
using ecs::Entity;
using ecs::World;

void TransformSystem3D::attach(World& w, Entity e, vec3 pos, quat rot, vec3 scale) {
    Transform3D t;
    t.position = pos; t.rotation = rot; t.scale = scale; t.dirty = 1;
    w.add<Transform3D>(e, t);
    w.add<WorldTransform3D>(e);
}

Entity TransformSystem3D::parentOf(World& w, Entity e) {
    Parent* p = w.get<Parent>(e);
    return p ? p->entity : Entity{};
}

bool TransformSystem3D::setParent(World& w, Entity child, Entity parent) {
    if (!parent.valid() || !w.alive(parent)) {
        if (w.has<Parent>(child)) { w.remove<Parent>(child); hierarchyDirty_ = true; }
        if (auto* t = w.get<Transform3D>(child)) t->dirty = 1;
        return true;
    }
    for (Entity p = parent; p.valid(); p = parentOf(w, p))
        if (p == child) return false;
    if (Parent* existing = w.get<Parent>(child)) existing->entity = parent;
    else w.add<Parent>(child, {parent});
    if (auto* t = w.get<Transform3D>(child)) t->dirty = 1;
    hierarchyDirty_ = true;
    return true;
}

void TransformSystem3D::rebuildOrder(World& w) {
    struct Item { uint32_t depth; Entity e; };
    std::vector<Item> items;
    w.each<Parent, Transform3D>([&](Entity e, Parent&, Transform3D&) {
        uint32_t depth = 0;
        for (Entity p = e;; ++depth) {
            Parent* pp = w.get<Parent>(p);
            if (!pp || !w.alive(pp->entity)) break;
            p = pp->entity;
        }
        items.push_back({depth, e});
    });
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.depth < y.depth; });
    ordered_.clear();
    for (const Item& it : items) ordered_.push_back(it.e);
    parentCount_ = w.count<Parent>();
    hierarchyDirty_ = false;
}

void TransformSystem3D::update(World& w) {
    ++frame_;
    const uint32_t frame = frame_;

    orphans_.clear();
    w.each<Parent>([&](Entity e, Parent& p) { if (!w.alive(p.entity)) orphans_.push_back(e); });
    for (Entity e : orphans_) setParent(w, e, Entity{});
    // Entities created or destroyed since last frame can change the child set without setParent.
    if (hierarchyDirty_ || !orphans_.empty() || parentCount_ != w.count<Parent>()) rebuildOrder(w);

    uint32_t n = 0;
    // Roots: archetypes with a transform but no Parent component.
    w.eachChunk<Transform3D, WorldTransform3D>([&](uint32_t count, const Entity* ents, Transform3D* t, WorldTransform3D* wt) {
        if (w.has<Parent>(ents[0])) return;   // whole archetype shares the component set
        for (uint32_t i = 0; i < count; ++i) {
            if (!t[i].dirty) continue;
            wt[i].m = t[i].local();
            wt[i].changedFrame = frame;
            t[i].dirty = 0;
            ++n;
        }
    });
    // Children, parents first: recompute when dirty or when the parent changed this frame.
    for (Entity e : ordered_) {
        Transform3D* t = w.get<Transform3D>(e);
        WorldTransform3D* wt = w.get<WorldTransform3D>(e);
        if (!t || !wt) continue;
        WorldTransform3D* pw = w.get<WorldTransform3D>(w.get<Parent>(e)->entity);
        const bool parentChanged = pw && pw->changedFrame == frame;
        if (!t->dirty && !parentChanged) continue;
        wt->m = pw ? pw->m * t->local() : t->local();
        wt->changedFrame = frame;
        t->dirty = 0;
        ++n;
    }
    recomputed_ = n;
}

}  // namespace mge::scene3d
