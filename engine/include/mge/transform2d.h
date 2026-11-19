#pragma once
// 2D transforms and parent/child hierarchy on top of the ECS.
//  - Transform2D: local position/rotation/scale, plus a dirty flag. Edit through the setters,
//    or call markDirty() after writing fields directly.
//  - WorldTransform2D: the resolved 2x3 affine, written only by TransformSystem2D::update.
//  - Only dirty transforms and their descendants are recomputed; a static scene costs one
//    linear pass of flag checks per frame.
//  - Roots are processed in parallel batches on the job system; children level by level.
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>
#include "mge/ecs.h"
#include "mge/math.h"

namespace mge::scene2d {

// | a c tx |
// | b d ty |
struct Affine2 {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    static Affine2 trs(vec2 t, float rot, vec2 s) {
        float cs = std::cos(rot), sn = std::sin(rot);
        return {cs * s.x, sn * s.x, -sn * s.y, cs * s.y, t.x, t.y};
    }
    vec2 apply(vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    vec2 applyDir(vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    vec2 translation() const { return {tx, ty}; }
    Affine2 inverse() const {
        float det = a * d - b * c;
        float id = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
        Affine2 r{d * id, -b * id, -c * id, a * id, 0, 0};
        r.tx = -(r.a * tx + r.c * ty);
        r.ty = -(r.b * tx + r.d * ty);
        return r;
    }
};
// parent * child: child space -> parent space -> world.
inline Affine2 operator*(const Affine2& p, const Affine2& ch) {
    return {p.a * ch.a + p.c * ch.b, p.b * ch.a + p.d * ch.b,
            p.a * ch.c + p.c * ch.d, p.b * ch.c + p.d * ch.d,
            p.a * ch.tx + p.c * ch.ty + p.tx, p.b * ch.tx + p.d * ch.ty + p.ty};
}

struct Transform2D {
    vec2 position{};
    float rotation = 0;   // radians, counter-clockwise
    vec2 scale{1, 1};
    uint32_t dirty = 1;

    void setPosition(vec2 p) { position = p; dirty = 1; }
    void setRotation(float r) { rotation = r; dirty = 1; }
    void setScale(vec2 s) { scale = s; dirty = 1; }
    void markDirty() { dirty = 1; }
    Affine2 local() const { return Affine2::trs(position, rotation, scale); }
};

struct WorldTransform2D {
    Affine2 m;
    uint32_t changedFrame = 0;   // frame it last changed; renderers use it to skip re-uploads
};

struct Parent { ecs::Entity entity; };

// Display name (editor hierarchy, lookups by name). UTF-8, NUL-terminated, max 31 bytes.
struct Name {
    char text[32] = {};
    // Copies `s`, cut at 31 bytes without splitting a UTF-8 sequence.
    static Name of(std::string_view s);
    std::string_view view() const { return {text, ::strnlen(text, sizeof text)}; }
};

class TransformSystem2D {
public:
    // Adds Transform2D + WorldTransform2D.
    static void attach(ecs::World& w, ecs::Entity e, vec2 pos = {}, float rot = 0, vec2 scale = {1, 1});
    // parent invalid = detach (becomes a root). Returns false (and changes nothing) on a cycle.
    bool setParent(ecs::World& w, ecs::Entity child, ecs::Entity parent);
    ecs::Entity parentOf(ecs::World& w, ecs::Entity e);
    // Parent components were written directly (editor, loads): rebuild the child order next update.
    void markHierarchyDirty() { hierarchyDirty_ = true; }

    // Resolve world transforms. Children of destroyed parents are detached and become roots.
    void update(ecs::World& w);

    uint32_t frame() const { return frame_; }
    uint32_t lastRecomputed() const { return recomputed_; }

    // Roots with more entities than this are split across job workers.
    uint32_t parallelThreshold = 4096;

private:
    void rebuildOrder(ecs::World& w);

    std::vector<ecs::Entity> ordered_;        // children sorted by depth (parents before children)
    std::vector<ecs::Entity> orphans_;
    bool hierarchyDirty_ = true;
    uint32_t frame_ = 0;
    uint32_t recomputed_ = 0;
};

}  // namespace mge::scene2d
