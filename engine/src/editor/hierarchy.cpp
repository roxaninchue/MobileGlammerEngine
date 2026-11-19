#include "mge/editor/hierarchy.h"
#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace mge::editor {
using ecs::Entity;
using scene2d::Affine2;
using scene2d::Name;
using scene2d::Parent;
using scene2d::Transform2D;

// ------------------------------------------------------------------ Selection
void Selection::toggle(Uid uid) {
    auto it = std::find(uids_.begin(), uids_.end(), uid);
    if (it != uids_.end()) uids_.erase(it);
    else if (uid != kNoUid) uids_.push_back(uid);
}

bool Selection::contains(Uid uid) const { return std::find(uids_.begin(), uids_.end(), uid) != uids_.end(); }

void Selection::prune(const SceneDoc& doc) {
    uids_.erase(std::remove_if(uids_.begin(), uids_.end(), [&](Uid u) { return !doc.entity(u).valid(); }), uids_.end());
}

// ------------------------------------------------------------------ helpers
void decompose(const Affine2& m, vec2& pos, float& rot, vec2& scale) {
    pos = {m.tx, m.ty};
    const float sx = std::sqrt(m.a * m.a + m.b * m.b);
    const float det = m.a * m.d - m.b * m.c;
    if (sx > 1e-8f) {
        rot = std::atan2(m.b, m.a);
        scale = {sx, det / sx};
    } else {   // degenerate x axis: take the rotation from the y axis
        const float sy = std::sqrt(m.c * m.c + m.d * m.d);
        rot = sy > 1e-8f ? std::atan2(-m.c, m.d) : 0.0f;
        scale = {0.0f, sy};
    }
}

namespace {
const serial::Registry::Type& typeOf(SceneDoc& doc, const char* name) {
    const serial::Registry::Type* t = doc.registry().find(name);
    MGE_VERIFY(t, "editor: component type not registered (registerScene2D + registerEditor)");
    return *t;
}

// " (n)" suffix of a copy name, or 0.
int copySuffix(std::string_view name, std::string_view& base) {
    base = name;
    if (name.size() < 4 || name.back() != ')') return 0;
    size_t open = name.rfind(" (");
    if (open == std::string_view::npos) return 0;
    int n = 0;
    for (size_t i = open + 2; i + 1 < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
        n = n * 10 + (name[i] - '0');
        if (n > 1000000) return 0;
    }
    if (open + 3 > name.size() - 1) return 0;
    base = name.substr(0, open);
    return n;
}
}  // namespace

std::string Hierarchy::nameOf(Uid uid) const {
    Entity e = doc_.entity(uid);
    if (e.valid())
        if (const Name* n = doc_.world().get<Name>(e); n && n->text[0]) return std::string(n->view());
    return "Entity " + std::to_string(uid);
}

Affine2 Hierarchy::worldMatrix(Uid uid) const {
    Affine2 m;
    for (Uid u = uid; u != kNoUid; u = doc_.parentOf(u)) {
        Entity e = doc_.entity(u);
        if (const Transform2D* t = e.valid() ? doc_.world().get<Transform2D>(e) : nullptr) m = t->local() * m;
    }
    return m;
}

// ------------------------------------------------------------------ tree
std::vector<TreeRow> Hierarchy::rows(const Selection& sel) const {
    struct Kid { int32_t order; Uid uid; };
    std::unordered_map<Uid, std::vector<Kid>> kids;
    ecs::World& w = doc_.world();
    w.each<EditorId>([&](Entity e, EditorId& id) {
        const Parent* p = w.get<Parent>(e);
        Uid parent = kNoUid;
        if (p) {
            const EditorId* pid = w.alive(p->entity) ? w.get<EditorId>(p->entity) : nullptr;
            parent = pid ? pid->uid : kNoUid;
        }
        const SiblingOrder* o = w.get<SiblingOrder>(e);
        kids[parent].push_back({o ? o->index : 0, id.uid});
    });
    for (auto& [_, v] : kids)
        std::sort(v.begin(), v.end(), [](const Kid& a, const Kid& b) { return a.order != b.order ? a.order < b.order : a.uid < b.uid; });

    std::vector<TreeRow> out;
    struct Frame { Uid parent; size_t next; uint32_t depth; };
    std::vector<Frame> stack{{kNoUid, 0, 0}};
    while (!stack.empty()) {
        Frame& f = stack.back();
        auto it = kids.find(f.parent);
        if (it == kids.end() || f.next >= it->second.size()) { stack.pop_back(); continue; }
        const Uid u = it->second[f.next++].uid;
        TreeRow r;
        r.uid = u;
        r.parent = f.parent;
        r.depth = f.depth;
        auto ck = kids.find(u);
        r.childCount = ck == kids.end() ? 0 : uint32_t(ck->second.size());
        r.expanded = expanded(u);
        r.selected = sel.contains(u);
        r.name = nameOf(u);
        const uint32_t depth = f.depth;   // `f` dangles once the stack grows
        out.push_back(std::move(r));
        if (out.back().expanded && out.back().childCount) stack.push_back({u, 0, depth + 1});
    }
    return out;
}

void Hierarchy::setExpanded(Uid uid, bool e) {
    if (e) expanded_.insert(uid);
    else expanded_.erase(uid);
    doc_.touch();
}

void Hierarchy::expandTo(Uid uid) {
    for (Uid p = doc_.parentOf(uid); p != kNoUid; p = doc_.parentOf(p)) expanded_.insert(p);
    doc_.touch();
}

// ------------------------------------------------------------------ edits
Uid Hierarchy::create(std::string_view name, Uid parent, vec2 pos, bool sprite) {
    if (parent != kNoUid && !doc_.entity(parent).valid()) return kNoUid;
    const std::vector<Uid> siblings = doc_.children(parent);
    EntitySnapshot es;
    es.uid = doc_.nextUid();
    es.components.push_back({&typeOf(doc_, "Name"), componentBytes(Name::of(name))});
    Transform2D t;
    t.position = pos;
    es.components.push_back({&typeOf(doc_, "Transform2D"), componentBytes(t)});
    SiblingOrder order;
    if (!siblings.empty())
        if (const SiblingOrder* last = doc_.world().get<SiblingOrder>(doc_.entity(siblings.back())))
            order.index = last->index + 1;
    es.components.push_back({&typeOf(doc_, "SiblingOrder"), componentBytes(order)});
    if (parent != kNoUid) es.components.push_back({&typeOf(doc_, "Parent"), componentBytes(Parent{Entity{parent, 0}})});
    if (sprite)
        if (const serial::Registry::Type* st = doc_.registry().find("SpriteRenderer"))
            es.components.push_back({st, componentBytes(render::SpriteRenderer{})});
    SubtreeSnapshot snap;
    snap.entities.push_back(std::move(es));
    const Uid uid = snap.entities[0].uid;
    if (!stack_.execute(SpawnCommand::spawn(std::move(snap), "Create " + std::string(name)))) return kNoUid;
    if (parent != kNoUid) expandTo(uid);
    return uid;
}

bool Hierarchy::rename(Uid uid, std::string_view name) {
    if (!doc_.entity(uid).valid()) return false;
    return stack_.execute(std::make_unique<SetComponentCommand>(doc_, uid, typeOf(doc_, "Name"),
                                                                componentBytes(Name::of(name)), "Rename"));
}

std::string Hierarchy::uniqueCopyName(Uid parent, std::string_view name) const {
    std::string_view base;
    copySuffix(name, base);
    std::unordered_set<std::string> taken;
    for (Uid s : doc_.children(parent)) taken.insert(nameOf(s));
    for (int n = 2;; ++n) {
        std::string candidate = std::string(base) + " (" + std::to_string(n) + ")";
        candidate = std::string(Name::of(candidate).view());
        if (!taken.count(candidate)) return candidate;
    }
}

std::vector<Uid> Hierarchy::duplicate(const std::vector<Uid>& roots) {
    // Only top-most selected entities: a child selected with its parent is copied once.
    std::vector<Uid> tops;
    for (Uid r : roots) {
        if (!doc_.entity(r).valid()) continue;
        bool covered = false;
        for (Uid o : roots) covered |= o != r && doc_.isAncestor(o, r);
        if (!covered && std::find(tops.begin(), tops.end(), r) == tops.end()) tops.push_back(r);
    }
    std::vector<Uid> made;
    if (tops.empty() || stack_.locked()) return made;
    const auto& nameT = typeOf(doc_, "Name");
    const auto& orderT = typeOf(doc_, "SiblingOrder");
    Uid next = doc_.nextUid();
    stack_.begin(tops.size() == 1 ? "Duplicate " + nameOf(tops[0]) : "Duplicate " + std::to_string(tops.size()));
    for (Uid root : tops) {
        SubtreeSnapshot snap = doc_.capture(root);
        std::unordered_map<Uid, Uid> remap;
        for (auto& es : snap.entities) remap[es.uid] = next++;
        for (auto& es : snap.entities) {
            es.uid = remap[es.uid];
            for (auto& c : es.components)
                for (const auto& f : c.type->fields) {
                    if (f.type != serial::FieldType::Entity) continue;
                    Entity ref;
                    std::memcpy(&ref, c.bytes.data() + f.offset, sizeof ref);
                    auto it = remap.find(ref.index);
                    if (it != remap.end()) {   // reference inside the copied subtree: point at the copy
                        ref.index = it->second;
                        std::memcpy(c.bytes.data() + f.offset, &ref, sizeof ref);
                    }
                }
        }
        const Uid parent = doc_.parentOf(root);
        const std::string copyName = uniqueCopyName(parent, nameOf(root));
        bool named = false;
        for (auto& c : snap.entities[0].components)
            if (c.type == &nameT) { c.bytes = componentBytes(Name::of(copyName)); named = true; }
        if (!named) snap.entities[0].components.push_back({&nameT, componentBytes(Name::of(copyName))});
        bool ordered = false;
        for (auto& c : snap.entities[0].components) ordered |= c.type == &orderT;
        if (!ordered) snap.entities[0].components.push_back({&orderT, componentBytes(SiblingOrder{})});
        const Uid copy = snap.entities[0].uid;
        if (!stack_.execute(SpawnCommand::spawn(std::move(snap), "Duplicate"))) continue;
        // Right after the original.
        std::vector<Uid> order;
        for (Uid s : doc_.children(parent)) {
            if (s == copy) continue;
            order.push_back(s);
            if (s == root) order.push_back(copy);
        }
        renumber(order);
        made.push_back(copy);
    }
    stack_.end();
    return made;
}

bool Hierarchy::remove(const std::vector<Uid>& roots) {
    std::vector<Uid> alive;
    for (Uid r : roots) if (doc_.entity(r).valid()) alive.push_back(r);
    if (alive.empty()) return false;
    const std::string label = alive.size() == 1 ? "Delete " + nameOf(alive[0]) : "Delete " + std::to_string(alive.size());
    return stack_.execute(SpawnCommand::despawn(std::move(alive), label));
}

void Hierarchy::renumber(const std::vector<Uid>& order) {
    const auto& orderT = typeOf(doc_, "SiblingOrder");
    for (size_t i = 0; i < order.size(); ++i) {
        const SiblingOrder* cur = doc_.world().get<SiblingOrder>(doc_.entity(order[i]));
        if (cur && cur->index == int32_t(i)) continue;
        stack_.execute(std::make_unique<SetComponentCommand>(doc_, order[i], orderT,
                                                             componentBytes(SiblingOrder{int32_t(i)}), "Order"));
    }
}

bool Hierarchy::reparent(Uid uid, Uid parent, int index, bool keepWorld) {
    if (!doc_.entity(uid).valid() || stack_.locked()) return false;
    if (parent != kNoUid && (!doc_.entity(parent).valid() || doc_.isAncestor(uid, parent))) return false;
    const Uid oldParent = doc_.parentOf(uid);
    const bool move = oldParent != parent;
    stack_.begin(move ? "Reparent " + nameOf(uid) : "Reorder " + nameOf(uid));
    if (move) {
        if (keepWorld && doc_.world().has<Transform2D>(doc_.entity(uid))) {
            const Affine2 world = worldMatrix(uid);
            const Affine2 local = parent != kNoUid ? worldMatrix(parent).inverse() * world : world;
            Transform2D t = *doc_.world().get<Transform2D>(doc_.entity(uid));
            decompose(local, t.position, t.rotation, t.scale);
            t.dirty = 1;
            stack_.execute(std::make_unique<SetComponentCommand>(doc_, uid, typeOf(doc_, "Transform2D"),
                                                                 componentBytes(t), "Keep world pose"));
        }
        const auto& parentT = typeOf(doc_, "Parent");
        if (parent == kNoUid) stack_.execute(std::make_unique<RemoveComponentCommand>(uid, parentT, "Unparent"));
        else stack_.execute(std::make_unique<SetComponentCommand>(doc_, uid, parentT, componentBytes(Parent{Entity{parent, 0}}), "Parent"));
    }
    std::vector<Uid> order;
    for (Uid s : doc_.children(parent)) if (s != uid) order.push_back(s);
    const size_t at = index < 0 || size_t(index) > order.size() ? order.size() : size_t(index);
    order.insert(order.begin() + ptrdiff_t(at), uid);
    renumber(order);
    stack_.end();
    if (parent != kNoUid) expandTo(uid);
    return true;
}

}  // namespace mge::editor
