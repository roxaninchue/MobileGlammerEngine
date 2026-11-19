#pragma once
// Editor hierarchy (M2 #22): the scene tree as the hierarchy panel shows it, plus the edits it
// makes. Every edit goes through the CommandStack, so each one is a single undo step.
//  - Tree rows are flattened depth-first in sibling order, collapsed subtrees skipped.
//  - Reparenting keeps the entity where it is on screen: its local transform is recomputed
//    against the new parent (rotation + scale decomposed; a skewing parent loses the skew).
//  - Sibling order is a SiblingOrder index; moves renumber only the siblings that change.
// Pure CPU code in mge_core, covered by host tests.
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "mge/editor/command.h"
#include "mge/render/sprite_system.h"

namespace mge::editor {

struct TreeRow {
    Uid uid = kNoUid;
    Uid parent = kNoUid;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    bool expanded = false;
    bool selected = false;
    std::string name;
};

class Selection {
public:
    void set(Uid uid) { uids_.clear(); if (uid != kNoUid) uids_.push_back(uid); }
    void toggle(Uid uid);
    void clear() { uids_.clear(); }
    bool contains(Uid uid) const;
    Uid primary() const { return uids_.empty() ? kNoUid : uids_.back(); }   // last one picked
    const std::vector<Uid>& all() const { return uids_; }
    bool empty() const { return uids_.empty(); }
    // Drops ids that no longer exist (after undo of a create, delete...).
    void prune(const SceneDoc& doc);

private:
    std::vector<Uid> uids_;
};

class Hierarchy {
public:
    explicit Hierarchy(CommandStack& stack) : stack_(stack), doc_(stack.doc()) {}

    std::vector<TreeRow> rows(const Selection& sel) const;
    void setExpanded(Uid uid, bool expanded);
    bool expanded(Uid uid) const { return expanded_.count(uid) != 0; }
    void expandTo(Uid uid);   // expands every ancestor so `uid` is visible

    // Display name (Name component, or "Entity <id>").
    std::string nameOf(Uid uid) const;

    // New entity with Name, Transform2D (at local `pos`) and optionally a white sprite, as the
    // last child of `parent` (or last root). Returns its id (kNoUid if the parent is unknown
    // or editing is locked).
    Uid create(std::string_view name, Uid parent = kNoUid, vec2 pos = {}, bool sprite = true);
    bool rename(Uid uid, std::string_view name);
    // Copies of each subtree (descendants and internal references included) with fresh ids,
    // placed right after their originals, named "<name> (2)", "(3)"... Returns the new roots.
    std::vector<Uid> duplicate(const std::vector<Uid>& roots);
    bool remove(const std::vector<Uid>& roots);
    // Moves `uid` under `parent` (kNoUid = root) at sibling position `index` (-1 = last).
    // Refuses cycles. Keeps the world pose when `keepWorld`.
    bool reparent(Uid uid, Uid parent, int index = -1, bool keepWorld = true);
    // Moves within the same parent.
    bool reorder(Uid uid, int index) { return reparent(uid, doc_.parentOf(uid), index, false); }

    // Local -> world of `uid` computed from Transform2D components up the chain (does not need
    // TransformSystem2D::update to have run).
    scene2d::Affine2 worldMatrix(Uid uid) const;

private:
    // Renumbers `order` (all children of one parent, in the wanted order) inside the open
    // transaction, touching only entries whose index changes.
    void renumber(const std::vector<Uid>& order);
    std::string uniqueCopyName(Uid parent, std::string_view base) const;

    CommandStack& stack_;
    SceneDoc& doc_;
    std::unordered_set<Uid> expanded_;
};

// Bytes in SceneDoc encoding for a plain component value (entity fields must already hold
// Entity{uid, 0}). Used to build commands without touching the world.
template <class T> std::vector<uint8_t> componentBytes(const T& v) {
    std::vector<uint8_t> b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

// Decomposes an affine without skew into position / rotation / scale (scale.y carries a mirror).
void decompose(const scene2d::Affine2& m, vec2& pos, float& rot, vec2& scale);

}  // namespace mge::editor
