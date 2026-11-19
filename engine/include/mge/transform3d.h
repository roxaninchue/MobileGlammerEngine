#pragma once
// 3D transforms and parent/child hierarchy on top of the ECS (M3 #26), the 3D twin of
// transform2d.h:
//  - Transform3D: local position / rotation (quaternion) / scale plus a dirty flag. Edit through
//    the setters, or call markDirty() after writing fields directly.
//  - WorldTransform3D: the resolved matrix, written only by TransformSystem3D::update.
//  - Parents are the same Parent component the 2D hierarchy uses (an entity is 2D or 3D, the
//    editor's hierarchy works on both). Only dirty transforms and their descendants are
//    recomputed; a static scene costs one pass of flag checks.
#include <vector>
#include "mge/ecs.h"
#include "mge/math.h"
#include "mge/transform2d.h"   // Parent, Name

namespace mge::scene3d {

using scene2d::Name;
using scene2d::Parent;

struct Transform3D {
    vec3 position{};
    uint32_t dirty = 1;
    quat rotation{};
    vec3 scale{1, 1, 1};

    void setPosition(vec3 p) { position = p; dirty = 1; }
    void setRotation(quat r) { rotation = r; dirty = 1; }
    void setScale(vec3 s) { scale = s; dirty = 1; }
    void markDirty() { dirty = 1; }
    mat4 local() const { return mat4::trs(position, normalize(rotation), scale); }
};

struct WorldTransform3D {
    mat4 m;
    uint32_t changedFrame = 0;   // frame it last changed; renderers use it to skip re-uploads
    vec3 position() const { return m.c[3].xyz(); }
};

class TransformSystem3D {
public:
    // Adds Transform3D + WorldTransform3D.
    static void attach(ecs::World& w, ecs::Entity e, vec3 pos = {}, quat rot = {}, vec3 scale = {1, 1, 1});
    // parent invalid = detach (becomes a root). Returns false (and changes nothing) on a cycle.
    bool setParent(ecs::World& w, ecs::Entity child, ecs::Entity parent);
    static ecs::Entity parentOf(ecs::World& w, ecs::Entity e);
    void markHierarchyDirty() { hierarchyDirty_ = true; }

    // Resolve world transforms. Children of destroyed parents are detached and become roots.
    void update(ecs::World& w);

    uint32_t frame() const { return frame_; }
    uint32_t lastRecomputed() const { return recomputed_; }

private:
    void rebuildOrder(ecs::World& w);

    std::vector<ecs::Entity> ordered_;   // 3D children, parents before children
    std::vector<ecs::Entity> orphans_;
    size_t parentCount_ = 0;
    bool hierarchyDirty_ = true;
    uint32_t frame_ = 0;
    uint32_t recomputed_ = 0;
};

}  // namespace mge::scene3d
