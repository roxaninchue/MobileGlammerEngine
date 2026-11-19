#include "test_main.h"
#include "mge/editor/command.h"
#include "mge/serialize.h"
#include "mge/transform2d.h"
#include <cmath>
#include <cstring>
using namespace mge;
using namespace mge::editor;
using namespace mge::scene2d;
using ecs::Entity;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

struct Fixture {
    serial::Registry reg;
    ecs::World world;
    TransformSystem2D ts;
    Fixture() { serial::registerScene2D(reg); registerEditor(reg); }
};

Uid makeNode(SceneDoc& doc, TransformSystem2D& ts, vec2 pos, Uid parent = kNoUid) {
    Entity e = doc.create();
    ts.attach(doc.world(), e, pos);
    if (parent) ts.setParent(doc.world(), e, doc.entity(parent));
    return doc.uidOf(e);
}

// Command that moves an entity's Transform2D to `pos` (what a gizmo drag pushes every frame).
CommandPtr moveTo(SceneDoc& doc, Uid uid, vec2 pos, uint64_t mergeKey = 0) {
    const auto& t = *doc.registry().find("Transform2D");
    std::vector<uint8_t> b = doc.readComponent(uid, t);
    Transform2D tr;
    std::memcpy(&tr, b.data(), sizeof tr);
    tr.position = pos;
    std::memcpy(b.data(), &tr, sizeof tr);
    auto c = std::make_unique<SetComponentCommand>(doc, uid, t, std::move(b), "Move");
    c->setMergeKey(mergeKey);
    return c;
}

vec2 posOf(SceneDoc& doc, Uid uid) { return doc.world().get<Transform2D>(doc.entity(uid))->position; }
}  // namespace

TEST(editor_doc_ids_stable_and_adopt) {
    Fixture f;
    Entity loose = f.world.create();   // made behind the document's back
    SceneDoc doc(f.world, f.reg, &f.ts);
    CHECK(doc.uidOf(loose) == 1);
    Uid a = makeNode(doc, f.ts, {1, 0});
    Uid b = makeNode(doc, f.ts, {2, 0}, a);
    CHECK(a == 2 && b == 3);
    CHECK(doc.parentOf(b) == a && doc.isAncestor(a, b) && !doc.isAncestor(b, a));
    CHECK(doc.children(kNoUid).size() == 2 && doc.children(a).size() == 1);
    CHECK(doc.subtree(a).size() == 2 && doc.subtree(a)[0] == a);
    // Duplicate ids (e.g. a pasted scene) are re-assigned by adopt().
    f.world.get<EditorId>(doc.entity(a))->uid = b;
    doc.adopt();
    CHECK(doc.uidOf(doc.entity(b)) == b);
    CHECK(doc.uidOf(loose) == 1);
    CHECK(doc.nextUid() == 5);
}

TEST(editor_undo_redo_set_component) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid a = makeNode(doc, f.ts, {0, 0});
    CHECK(!stack.canUndo() && !stack.isDirty());
    CHECK(stack.execute(moveTo(doc, a, {3, 4})));
    CHECK(near(posOf(doc, a).x, 3) && stack.isDirty() && stack.undoLabel() == "Move");
    CHECK(stack.undo());
    CHECK(near(posOf(doc, a).x, 0) && !stack.isDirty());
    CHECK(stack.canRedo() && stack.redo());
    CHECK(near(posOf(doc, a).y, 4));
    f.ts.update(f.world);
    CHECK(near(f.world.get<WorldTransform2D>(doc.entity(a))->m.tx, 3));   // hooks mark the transform dirty
    // No-op edits never become undo steps.
    CHECK(stack.execute(moveTo(doc, a, {3, 4})) && stack.size() == 1);
}

TEST(editor_merge_and_seal) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid a = makeNode(doc, f.ts, {0, 0});
    for (int i = 1; i <= 30; ++i) stack.execute(moveTo(doc, a, {float(i), 0}, /*key*/ 7));   // one drag
    CHECK(stack.size() == 1 && near(posOf(doc, a).x, 30));
    stack.seal();                                                                         // finger lifted
    for (int i = 1; i <= 5; ++i) stack.execute(moveTo(doc, a, {30, float(i)}, 7));         // second drag
    CHECK(stack.size() == 2);
    stack.undo();
    CHECK(near(posOf(doc, a).x, 30) && near(posOf(doc, a).y, 0));
    stack.undo();
    CHECK(near(posOf(doc, a).x, 0));
    // Different keys never merge.
    stack.execute(moveTo(doc, a, {1, 1}, 1));
    stack.execute(moveTo(doc, a, {2, 2}, 2));
    CHECK(stack.size() == 2 && !stack.canRedo());
}

TEST(editor_delete_subtree_undo_restores_ids_and_refs) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid root = makeNode(doc, f.ts, {10, 0});
    Uid child = makeNode(doc, f.ts, {1, 0}, root);
    Uid grand = makeNode(doc, f.ts, {0, 1}, child);
    Uid other = makeNode(doc, f.ts, {-5, 0});
    stack.execute(moveTo(doc, grand, {0, 2}));   // recorded before the delete, replayed after its undo
    CHECK(stack.execute(SpawnCommand::despawn({root}, "Delete")));
    CHECK(!doc.entity(root).valid() && !doc.entity(child).valid() && !doc.entity(grand).valid());
    CHECK(doc.entity(other).valid() && f.world.aliveCount() == 1);
    CHECK(stack.undo());
    CHECK(f.world.aliveCount() == 4);
    CHECK(doc.parentOf(child) == root && doc.parentOf(grand) == child);
    f.ts.update(f.world);
    const Affine2& m = f.world.get<WorldTransform2D>(doc.entity(grand))->m;
    CHECK(near(m.tx, 11) && near(m.ty, 2));
    CHECK(stack.undo());   // the move before the delete still finds the recreated entity
    CHECK(near(posOf(doc, grand).y, 1));
    CHECK(stack.redo() && stack.redo());
    CHECK(f.world.aliveCount() == 1);
    CHECK(stack.undo());
    CHECK(f.world.aliveCount() == 4);
}

TEST(editor_spawn_and_component_add_remove) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid a = makeNode(doc, f.ts, {0, 0});
    Uid b = makeNode(doc, f.ts, {4, 0});
    // Spawn a copy of a's subtree under fresh ids.
    SubtreeSnapshot s = doc.capture(a);
    s.entities[0].uid = doc.nextUid();
    CHECK(stack.execute(SpawnCommand::spawn(s, "Duplicate")));
    CHECK(f.world.aliveCount() == 3);
    CHECK(!doc.restore(s));   // already alive: refused, nothing created
    CHECK(f.world.aliveCount() == 3);
    stack.undo();
    CHECK(f.world.aliveCount() == 2);
    stack.redo();
    // Parent: add via SetComponent (did not exist), then remove; undo of each.
    const auto& parentT = *f.reg.find("Parent");
    std::vector<uint8_t> bytes(sizeof(Parent));
    Entity enc{a, 0};   // SceneDoc encoding: entity refs are ids
    std::memcpy(bytes.data(), &enc, sizeof enc);
    CHECK(stack.execute(std::make_unique<SetComponentCommand>(doc, b, parentT, bytes, "Parent")));
    CHECK(doc.parentOf(b) == a);
    f.ts.update(f.world);
    CHECK(f.ts.parentOf(f.world, doc.entity(b)) == doc.entity(a));
    CHECK(stack.execute(std::make_unique<RemoveComponentCommand>(b, parentT)));
    CHECK(doc.parentOf(b) == kNoUid);
    stack.undo();
    CHECK(doc.parentOf(b) == a);
    stack.undo();
    CHECK(doc.parentOf(b) == kNoUid && !f.world.has<Parent>(doc.entity(b)));
    // Removing a component that is not there fails and records nothing.
    const size_t n = stack.size();
    CHECK(!stack.execute(std::make_unique<RemoveComponentCommand>(b, parentT)));
    CHECK(stack.size() == n);
}

TEST(editor_transactions_group_and_nest) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid a = makeNode(doc, f.ts, {0, 0});
    Uid b = makeNode(doc, f.ts, {0, 0});
    stack.begin("Align");
    stack.execute(moveTo(doc, a, {1, 1}));
    stack.begin("inner");
    stack.execute(moveTo(doc, b, {2, 2}));
    stack.end();
    CHECK(stack.inTransaction() && !stack.canUndo());
    stack.end();
    CHECK(stack.size() == 1 && stack.undoLabel() == "Align");
    stack.undo();
    CHECK(near(posOf(doc, a).x, 0) && near(posOf(doc, b).x, 0));
    stack.redo();
    CHECK(near(posOf(doc, a).x, 1) && near(posOf(doc, b).x, 2));
    stack.begin("nothing");
    stack.end();
    CHECK(stack.size() == 1);
}

TEST(editor_save_point_and_caps) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStackConfig cfg;
    cfg.maxSteps = 4;
    CommandStack stack(doc, cfg);
    Uid a = makeNode(doc, f.ts, {0, 0});
    stack.execute(moveTo(doc, a, {1, 0}));
    stack.markSaved();
    CHECK(!stack.isDirty());
    stack.execute(moveTo(doc, a, {2, 0}));
    CHECK(stack.isDirty());
    stack.undo();
    CHECK(!stack.isDirty());
    stack.undo();
    CHECK(stack.isDirty());
    stack.redo();
    CHECK(!stack.isDirty());
    // Merging into the saved step makes it dirty.
    stack.execute(moveTo(doc, a, {5, 0}, 9));
    stack.markSaved();
    stack.execute(moveTo(doc, a, {6, 0}, 9));
    CHECK(stack.isDirty());
    // Overwriting the redo branch that held the save point: dirty until saved again.
    stack.markSaved();
    stack.undo();
    stack.seal();
    stack.execute(moveTo(doc, a, {7, 0}));
    CHECK(stack.isDirty());
    // Step cap: oldest steps drop, newest stay undoable.
    for (int i = 0; i < 10; ++i) stack.execute(moveTo(doc, a, {float(10 + i), 0}));
    CHECK(stack.size() == 4);
    for (int i = 0; i < 4; ++i) CHECK(stack.undo());
    CHECK(!stack.canUndo() && near(posOf(doc, a).x, 15));
    CHECK(stack.isDirty());
    // Byte cap.
    CommandStackConfig small;
    small.maxBytes = 1;
    CommandStack tiny(doc, small);
    tiny.execute(moveTo(doc, a, {1, 1}));
    tiny.execute(moveTo(doc, a, {2, 2}));
    CHECK(tiny.size() == 1);   // always keeps the latest step
}

TEST(editor_lock_blocks_edits) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    CommandStack stack(doc);
    Uid a = makeNode(doc, f.ts, {0, 0});
    stack.execute(moveTo(doc, a, {1, 0}));
    stack.lock(true);
    CHECK(!stack.execute(moveTo(doc, a, {2, 0})) && near(posOf(doc, a).x, 1));
    CHECK(!stack.canUndo() && !stack.undo());
    stack.lock(false);
    CHECK(stack.undo() && near(posOf(doc, a).x, 0));
}

TEST(editor_ids_survive_save_load) {
    Fixture f;
    SceneDoc doc(f.world, f.reg, &f.ts);
    Uid a = makeNode(doc, f.ts, {0, 0});
    Uid b = makeNode(doc, f.ts, {1, 0}, a);
    std::string js = serial::saveJson(f.world, f.reg);
    CHECK(js.find("\"EditorId\":{\"uid\":2}") != std::string::npos);
    ecs::World w2;
    TransformSystem2D ts2;
    CHECK(serial::loadJson(w2, f.reg, js));
    SceneDoc doc2(w2, f.reg, &ts2);
    CHECK(doc2.entity(a).valid() && doc2.parentOf(b) == a && doc2.nextUid() == b + 1);
}
