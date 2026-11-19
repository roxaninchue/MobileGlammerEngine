#include "mge/editor/scene_doc.h"
#include <algorithm>
#include <unordered_set>
#include "mge/log.h"

namespace mge::editor {
using ecs::Entity;
using scene2d::Parent;
using serial::FieldType;
using serial::Registry;

void registerEditor(Registry& reg) {
    if (!reg.find("EditorId")) reg.add<EditorId>("EditorId", {MGE_FIELD(EditorId, uid, U32)}).editorHidden = true;
    if (!reg.find("SiblingOrder"))
        reg.add<SiblingOrder>("SiblingOrder", {MGE_FIELD(SiblingOrder, index, I32)}).editorHidden = true;
    // Saved scenes list entities by editor id: stable files however the ECS reused its slots.
    reg.entityOrder = [](ecs::World& w, Entity e) -> uint64_t {
        const EditorId* id = w.get<EditorId>(e);
        return id ? id->uid : UINT64_MAX;
    };
}

size_t SubtreeSnapshot::bytes() const {
    size_t b = sizeof(*this) + entities.capacity() * sizeof(EntitySnapshot);
    for (const auto& e : entities)
        for (const auto& c : e.components) b += sizeof(ComponentBlob) + c.bytes.capacity();
    return b;
}

SceneDoc::SceneDoc(ecs::World& world, const Registry& reg, scene2d::TransformSystem2D* transforms)
    : world_(world), reg_(reg), transforms_(transforms) {
    adopt();
}

Entity SceneDoc::create(Uid uid) {
    if (uid == kNoUid || entity(uid).valid()) uid = next_;
    Entity e = world_.create();
    world_.add<EditorId>(e, {uid});
    byUid_[uid] = e;
    next_ = std::max(next_, uid + 1);
    ++revision_;
    return e;
}

void SceneDoc::adopt() {
    byUid_.clear();
    std::vector<Entity> missing;
    world_.eachEntity([&](Entity e) {
        const EditorId* id = world_.get<EditorId>(e);
        if (!id || id->uid == kNoUid || byUid_.count(id->uid)) { missing.push_back(e); return; }
        byUid_[id->uid] = e;
        next_ = std::max(next_, id->uid + 1);
    });
    std::sort(missing.begin(), missing.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (Entity e : missing) {
        world_.add<EditorId>(e, {next_});
        byUid_[next_++] = e;
    }
    hierarchyChanged();
}

Entity SceneDoc::entity(Uid uid) const {
    auto it = byUid_.find(uid);
    return it != byUid_.end() && world_.alive(it->second) ? it->second : Entity{};
}

Uid SceneDoc::uidOf(Entity e) {
    const EditorId* id = world_.alive(e) ? world_.get<EditorId>(e) : nullptr;
    return id ? id->uid : kNoUid;
}

Uid SceneDoc::parentOf(Uid uid) {
    Entity e = entity(uid);
    if (!e.valid()) return kNoUid;
    const Parent* p = world_.get<Parent>(e);
    return p ? uidOf(p->entity) : kNoUid;
}

bool SceneDoc::isAncestor(Uid ancestor, Uid uid) {
    for (Uid u = uid; u != kNoUid; u = parentOf(u))
        if (u == ancestor) return true;
    return false;
}

std::vector<Uid> SceneDoc::children(Uid uid) {
    struct Item { int32_t order; Uid uid; };
    std::vector<Item> items;
    world_.each<EditorId>([&](Entity e, EditorId& id) {
        const Parent* p = world_.get<Parent>(e);
        const Uid parent = p ? uidOf(p->entity) : kNoUid;
        if (parent != uid || id.uid == uid) return;
        const SiblingOrder* o = world_.get<SiblingOrder>(e);
        items.push_back({o ? o->index : 0, id.uid});
    });
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.order != b.order ? a.order < b.order : a.uid < b.uid; });
    std::vector<Uid> out;
    out.reserve(items.size());
    for (const Item& i : items) out.push_back(i.uid);
    return out;
}

std::vector<Uid> SceneDoc::subtree(Uid uid) {
    std::vector<Uid> out;
    if (!entity(uid).valid()) return out;
    // One pass to group children by parent, then a breadth-first walk (parents first).
    std::unordered_map<Uid, std::vector<Uid>> kids;
    world_.each<EditorId, Parent>([&](Entity, EditorId& id, Parent& p) { kids[uidOf(p.entity)].push_back(id.uid); });
    out.push_back(uid);
    for (size_t i = 0; i < out.size(); ++i) {
        auto it = kids.find(out[i]);
        if (it == kids.end()) continue;
        std::sort(it->second.begin(), it->second.end());
        for (Uid c : it->second) out.push_back(c);
    }
    return out;
}

void SceneDoc::encodeRefs(const Registry::Type& t, uint8_t* bytes) {
    for (const auto& f : t.fields) {
        if (f.type != FieldType::Entity) continue;
        Entity ref;
        std::memcpy(&ref, bytes + f.offset, sizeof ref);
        const Uid u = uidOf(ref);
        Entity enc{u == kNoUid ? UINT32_MAX : u, 0};
        std::memcpy(bytes + f.offset, &enc, sizeof enc);
    }
}

void SceneDoc::decodeRefs(const Registry::Type& t, uint8_t* bytes) const {
    for (const auto& f : t.fields) {
        if (f.type != FieldType::Entity) continue;
        Entity enc;
        std::memcpy(&enc, bytes + f.offset, sizeof enc);
        Entity ref = enc.index == UINT32_MAX ? Entity{} : entity(enc.index);
        std::memcpy(bytes + f.offset, &ref, sizeof ref);
    }
}

std::vector<uint8_t> SceneDoc::readComponent(Uid uid, const Registry::Type& t) {
    std::vector<uint8_t> out;
    Entity e = entity(uid);
    const void* p = e.valid() ? world_.getRaw(e, t.id) : nullptr;
    if (!p) return out;
    out.resize(t.size);
    std::memcpy(out.data(), p, t.size);
    encodeRefs(t, out.data());
    return out;
}

bool SceneDoc::writeComponent(Uid uid, const Registry::Type& t, const std::vector<uint8_t>& bytes) {
    Entity e = entity(uid);
    if (!e.valid() || bytes.size() != t.size) return false;
    const bool isParent = t.id == ecs::componentId<Parent>();
    auto* dst = static_cast<uint8_t*>(world_.addRaw(e, t.id));
    std::memcpy(dst, bytes.data(), t.size);
    decodeRefs(t, dst);
    if (t.onAdd) t.onAdd(world_, e);
    if (isParent) {
        if (auto* tr = world_.get<scene2d::Transform2D>(e)) tr->markDirty();
        hierarchyChanged();
    }
    ++revision_;
    return true;
}

bool SceneDoc::removeComponent(Uid uid, const Registry::Type& t) {
    Entity e = entity(uid);
    if (!e.valid() || !world_.getRaw(e, t.id)) return false;
    // ECS remove is templated; go through the type-erased path by rebuilding without it.
    world_.removeRaw(e, t.id);
    if (t.id == ecs::componentId<Parent>()) {
        if (auto* tr = world_.get<scene2d::Transform2D>(e)) tr->markDirty();
        hierarchyChanged();
    }
    ++revision_;
    return true;
}

SubtreeSnapshot SceneDoc::capture(Uid root) { return capture(std::vector<Uid>{root}); }

SubtreeSnapshot SceneDoc::capture(const std::vector<Uid>& roots) {
    SubtreeSnapshot s;
    std::unordered_set<Uid> seen;
    const ecs::ComponentId idComp = ecs::componentId<EditorId>();
    for (Uid r : roots) {
        if (seen.count(r)) continue;
        for (Uid u : subtree(r)) {
            if (!seen.insert(u).second) continue;
            EntitySnapshot es;
            es.uid = u;
            for (const auto& t : reg_.types()) {
                if (t.id == idComp) continue;   // the id is the snapshot's own uid
                std::vector<uint8_t> b = readComponent(u, t);
                if (!b.empty()) es.components.push_back({&t, std::move(b)});
            }
            s.entities.push_back(std::move(es));
        }
    }
    // A root listed after one of its ancestors was already pulled in through that ancestor;
    // a root listed before its ancestor must still come after it: re-sort parents first.
    std::unordered_map<Uid, size_t> depth;
    for (const auto& e : s.entities) {
        size_t d = 0;
        for (Uid p = parentOf(e.uid); p != kNoUid; p = parentOf(p)) ++d;
        depth[e.uid] = d;
    }
    std::stable_sort(s.entities.begin(), s.entities.end(),
                     [&](const EntitySnapshot& a, const EntitySnapshot& b) { return depth[a.uid] < depth[b.uid]; });
    return s;
}

bool SceneDoc::restore(const SubtreeSnapshot& s) {
    for (const auto& es : s.entities)
        if (es.uid == kNoUid || entity(es.uid).valid()) {
            MGE_LOGW("editor: restore of id %u refused, already alive", es.uid);
            return false;
        }
    // Entities first so references between them resolve, then components, then hooks.
    std::vector<Entity> made;
    made.reserve(s.entities.size());
    for (const auto& es : s.entities) made.push_back(create(es.uid));
    for (size_t i = 0; i < s.entities.size(); ++i)
        for (const auto& c : s.entities[i].components) {
            auto* dst = static_cast<uint8_t*>(world_.addRaw(made[i], c.type->id));
            std::memcpy(dst, c.bytes.data(), c.type->size);
            decodeRefs(*c.type, dst);
        }
    for (size_t i = 0; i < s.entities.size(); ++i)
        for (const auto& c : s.entities[i].components)
            if (c.type->onAdd) c.type->onAdd(world_, made[i]);
    hierarchyChanged();
    ++revision_;
    return true;
}

void SceneDoc::destroy(const SubtreeSnapshot& s) {
    for (auto it = s.entities.rbegin(); it != s.entities.rend(); ++it) {   // children first
        Entity e = entity(it->uid);
        if (e.valid()) world_.destroy(e);
        byUid_.erase(it->uid);
    }
    hierarchyChanged();
    ++revision_;
}

void SceneDoc::hierarchyChanged() {
    if (transforms_) transforms_->markHierarchyDirty();
}

}  // namespace mge::editor
