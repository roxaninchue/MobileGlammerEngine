#pragma once
// Editor scene document (M2 #21): the ECS world being edited, seen through stable editor ids.
//  - Every authored entity carries an EditorId (saved with the scene). Commands name entities
//    by that id, never by ecs::Entity, so undoing a delete brings back an entity that the
//    commands recorded before and after it still find, even though its handle changed.
//  - Snapshots hold the registry components of an entity subtree (entity + every descendant
//    through Parent) as raw bytes, with entity references rewritten to editor ids. Restoring
//    one recreates the entities with the same ids and runs the registry onAdd hooks, exactly
//    like a scene load. Runtime-only components (WorldTransform2D, RigidBody2D...) are not
//    captured; hooks and play mode rebuild them.
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "mge/ecs.h"
#include "mge/serialize.h"
#include "mge/transform2d.h"

namespace mge::editor {

using Uid = uint32_t;
constexpr Uid kNoUid = 0;

// Stable editor identity of an entity. 0 is never a valid id.
struct EditorId {
    Uid uid = kNoUid;
};

// Adds EditorId to the registry so ids survive save/load (call once per registry).
void registerEditor(serial::Registry& reg);

struct ComponentBlob {
    const serial::Registry::Type* type = nullptr;
    std::vector<uint8_t> bytes;   // entity fields hold Entity{uid, 0} (index = uid, UINT32_MAX = none)
};

struct EntitySnapshot {
    Uid uid = kNoUid;
    std::vector<ComponentBlob> components;
};

// Parents always come before their children.
struct SubtreeSnapshot {
    std::vector<EntitySnapshot> entities;
    size_t bytes() const;
};

class SceneDoc {
public:
    // `transforms` (optional) is told about hierarchy changes so its child order stays correct.
    SceneDoc(ecs::World& world, const serial::Registry& reg, scene2d::TransformSystem2D* transforms = nullptr);

    ecs::World& world() { return world_; }
    const serial::Registry& registry() const { return reg_; }
    scene2d::TransformSystem2D* transforms() { return transforms_; }

    // Creates an entity with a fresh id (or `uid` when given and free).
    ecs::Entity create(Uid uid = kNoUid);
    // Gives an id to every entity that has none and rebuilds the lookup (after a scene load or
    // anything that created entities behind the document's back). Duplicate ids get new ones.
    void adopt();

    ecs::Entity entity(Uid uid) const;   // invalid when unknown or dead
    Uid uidOf(ecs::Entity e);            // kNoUid when the entity has no EditorId
    Uid nextUid() const { return next_; }

    // Children (direct) of `uid`, or the roots when uid == kNoUid, in creation order of their ids.
    std::vector<Uid> children(Uid uid);
    // `uid` and all its descendants, parents first.
    std::vector<Uid> subtree(Uid uid);
    Uid parentOf(Uid uid);
    bool isAncestor(Uid ancestor, Uid uid);   // true when ancestor == uid too

    // Snapshots / restore (see the header comment). Restore fails (and changes nothing) if any
    // id in the snapshot is already alive.
    SubtreeSnapshot capture(Uid root);
    SubtreeSnapshot capture(const std::vector<Uid>& roots);   // several subtrees, overlap removed
    bool restore(const SubtreeSnapshot& s);
    void destroy(const SubtreeSnapshot& s);   // destroys every entity the snapshot names

    // Raw component bytes with entity references encoded as ids, and back.
    std::vector<uint8_t> readComponent(Uid uid, const serial::Registry::Type& t);
    bool writeComponent(Uid uid, const serial::Registry::Type& t, const std::vector<uint8_t>& bytes);
    bool removeComponent(Uid uid, const serial::Registry::Type& t);

    // Tells the transform system that parents changed (also done by the calls above).
    void hierarchyChanged();
    // Bumped on every change made through the document; UI panels refresh when it moves.
    uint64_t revision() const { return revision_; }
    void touch() { ++revision_; }

private:
    void encodeRefs(const serial::Registry::Type& t, uint8_t* bytes);
    void decodeRefs(const serial::Registry::Type& t, uint8_t* bytes) const;

    ecs::World& world_;
    const serial::Registry& reg_;
    scene2d::TransformSystem2D* transforms_;
    std::unordered_map<Uid, ecs::Entity> byUid_;
    Uid next_ = 1;
    uint64_t revision_ = 0;
};

}  // namespace mge::editor
