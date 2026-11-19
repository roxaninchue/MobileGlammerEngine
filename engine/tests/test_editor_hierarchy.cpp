#include "test_main.h"
#include "mge/editor/hierarchy.h"
#include "mge/editor/inspector.h"
#include "mge/render/sprite_system.h"
#include <cmath>
using namespace mge;
using namespace mge::editor;
using namespace mge::scene2d;

namespace {
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

struct Fixture {
    serial::Registry reg;
    ecs::World world;
    TransformSystem2D ts;
    std::unique_ptr<SceneDoc> doc;
    std::unique_ptr<CommandStack> stack;
    std::unique_ptr<Hierarchy> h;
    std::unique_ptr<Inspector> insp;
    Fixture() {
        serial::registerScene2D(reg);
        render::registerRender2D(reg);
        registerEditor(reg);
        doc = std::make_unique<SceneDoc>(world, reg, &ts);
        stack = std::make_unique<CommandStack>(*doc);
        h = std::make_unique<Hierarchy>(*stack);
        insp = std::make_unique<Inspector>(*stack);
    }
    std::vector<std::string> names(const std::vector<Uid>& uids) {
        std::vector<std::string> v;
        for (Uid u : uids) v.push_back(h->nameOf(u));
        return v;
    }
    vec2 worldPos(Uid u) {
        ts.update(world);
        return world.get<WorldTransform2D>(doc->entity(u))->m.translation();
    }
};
}  // namespace

TEST(hierarchy_create_rows_and_undo) {
    Fixture f;
    Uid a = f.h->create("Player", kNoUid, {1, 2});
    Uid b = f.h->create("Enemy");
    Uid c = f.h->create("Gun", a, {0.5f, 0});
    CHECK(a && b && c);
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"Player", "Enemy"}));
    CHECK(f.world.has<render::SpriteRenderer>(f.doc->entity(a)));
    Selection sel;
    sel.set(c);
    auto rows = f.h->rows(sel);   // creating a child expanded its parent
    CHECK(rows.size() == 3 && rows[0].name == "Player" && rows[0].childCount == 1 && rows[0].expanded);
    CHECK(rows[1].name == "Gun" && rows[1].depth == 1 && rows[1].selected && rows[2].name == "Enemy");
    f.h->setExpanded(a, false);
    CHECK(f.h->rows(sel).size() == 2);
    CHECK(near(f.worldPos(c).x, 1.5f));
    f.stack->undo();
    sel.prune(*f.doc);
    CHECK(sel.empty() && f.world.aliveCount() == 2);
    f.stack->redo();
    CHECK(f.doc->entity(c).valid() && f.doc->parentOf(c) == a);
}

TEST(hierarchy_rename_and_unicode) {
    Fixture f;
    Uid a = f.h->create("A");
    CHECK(f.h->rename(a, "Caf\xC3\xA9 \"boss\""));
    CHECK(f.h->nameOf(a) == "Caf\xC3\xA9 \"boss\"");
    f.stack->undo();
    CHECK(f.h->nameOf(a) == "A");
}

TEST(hierarchy_reparent_keeps_world_pose) {
    Fixture f;
    Uid p = f.h->create("Pivot", kNoUid, {10, 0});
    // Rotate and scale the parent so the conversion is not trivial.
    CHECK(f.insp->setField(p, "Transform2D", "rotation", "90"));
    CHECK(f.insp->setField(p, "Transform2D", "scale", "2 2"));
    Uid c = f.h->create("Child", kNoUid, {10, 4});
    const vec2 before = f.worldPos(c);
    CHECK(f.h->reparent(c, p));
    CHECK(f.doc->parentOf(c) == p);
    const vec2 after = f.worldPos(c);
    CHECK(near(before.x, after.x) && near(before.y, after.y));
    const Transform2D* t = f.world.get<Transform2D>(f.doc->entity(c));
    CHECK(near(t->position.x, 2) && near(t->position.y, 0) && near(t->scale.x, 0.5f) && near(t->rotation, -kPi / 2));
    CHECK(f.stack->size() == 5 && f.stack->undoLabel() == "Reparent Child");   // one undo step
    // Back to root, still in place.
    CHECK(f.h->reparent(c, kNoUid));
    CHECK(near(f.worldPos(c).x, 10) && near(f.worldPos(c).y, 4) && !f.world.has<Parent>(f.doc->entity(c)));
    f.stack->undo();
    f.stack->undo();
    CHECK(f.doc->parentOf(c) == kNoUid);
    CHECK(near(f.world.get<Transform2D>(f.doc->entity(c))->position.y, 4));
    // Cycles are refused.
    f.h->reparent(c, p);
    CHECK(!f.h->reparent(p, c) && !f.h->reparent(p, p));
}

TEST(hierarchy_reorder) {
    Fixture f;
    Uid a = f.h->create("A"), b = f.h->create("B"), c = f.h->create("C");
    (void)b;
    CHECK(f.h->reorder(c, 0));
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"C", "A", "B"}));
    CHECK(f.h->reorder(a, 99));
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"C", "B", "A"}));
    f.stack->undo();
    f.stack->undo();
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"A", "B", "C"}));
    const size_t n = f.stack->size();
    CHECK(f.h->reorder(a, 0) && f.stack->size() == n);   // no change, no undo step
}

TEST(hierarchy_duplicate_subtree_and_delete) {
    Fixture f;
    Uid a = f.h->create("Ship"), b = f.h->create("Rock");
    Uid gun = f.h->create("Gun", a, {1, 0});
    f.h->create("Barrel", gun, {1, 0});
    auto copies = f.h->duplicate({a, gun});   // gun is inside a: copied once, with a
    CHECK(copies.size() == 1);
    Uid copy = copies[0];
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"Ship", "Ship (2)", "Rock"}));
    CHECK(f.doc->subtree(copy).size() == 3 && f.doc->subtree(a).size() == 3);
    Uid copyGun = f.doc->children(copy)[0];
    CHECK(copyGun != gun && f.h->nameOf(copyGun) == "Gun" && f.doc->parentOf(copyGun) == copy);
    auto again = f.h->duplicate({copy});
    CHECK(f.h->nameOf(again[0]) == "Ship (3)");
    CHECK(f.stack->undoLabel() == "Duplicate Ship (2)");
    f.stack->undo();
    f.stack->undo();
    CHECK(f.world.aliveCount() == 4 && !f.doc->entity(copy).valid());
    f.stack->redo();
    CHECK(f.doc->entity(copy).valid() && f.doc->entity(copyGun).valid());
    CHECK(f.h->remove({a, b}));
    CHECK(f.world.aliveCount() == 3 && f.stack->undoLabel() == "Delete 2");
    f.stack->undo();
    CHECK(f.world.aliveCount() == 7 && f.doc->parentOf(gun) == a);
    CHECK((f.names(f.doc->children(kNoUid)) == std::vector<std::string>{"Ship", "Ship (2)", "Rock"}));
}

TEST(inspector_lists_formats_and_edits) {
    Fixture f;
    Uid a = f.h->create("A", kNoUid, {1.5f, -2});
    auto comps = f.insp->inspect(a);
    CHECK(comps.size() == 2 && comps[0].type == "Transform2D" && comps[1].type == "SpriteRenderer");
    CHECK(!comps[0].removable && comps[1].removable);
    CHECK(comps[0].fields[0].name == "position" && comps[0].fields[0].value == "1.5 -2");
    CHECK(comps[0].fields[1].hint == serial::FieldHint::Angle && comps[0].fields[1].value == "0");
    bool sawColor = false;
    for (auto& fl : comps[1].fields) if (fl.name == "color") { sawColor = true; CHECK(fl.value == "#FFFFFFFF"); }
    CHECK(sawColor);
    CHECK(f.insp->setField(a, "SpriteRenderer", "color", "#FF8000"));
    CHECK(f.world.get<render::SpriteRenderer>(f.doc->entity(a))->color == 0xFF0080FFu);
    CHECK(f.insp->setField(a, "SpriteRenderer", "visible", "false"));
    CHECK(f.world.get<render::SpriteRenderer>(f.doc->entity(a))->visible == 0);
    CHECK(f.insp->setField(a, "SpriteRenderer", "layer", "-12"));
    CHECK(f.insp->setField(a, "SpriteRenderer", "uvRect", "0, 0.5, 0.25, 1"));
    CHECK(f.world.get<render::SpriteRenderer>(f.doc->entity(a))->uvRect.y == 0.5f);
    // Bad input changes nothing and records nothing.
    const size_t n = f.stack->size();
    CHECK(!f.insp->setField(a, "SpriteRenderer", "layer", "40000"));
    CHECK(!f.insp->setField(a, "Transform2D", "position", "1"));
    CHECK(!f.insp->setField(a, "Transform2D", "position", "1 x"));
    CHECK(!f.insp->setField(a, "SpriteRenderer", "color", "#12"));
    CHECK(!f.insp->setField(a, "Transform2D", "nope", "1"));
    CHECK(!f.insp->setField(a, "Parent", "entity", "1"));   // hidden: the hierarchy owns it
    CHECK(f.stack->size() == n);
}

TEST(inspector_scrub_merges_and_components) {
    Fixture f;
    Uid a = f.h->create("A", kNoUid, {}, false);
    CHECK(f.insp->inspect(a).size() == 1);
    CHECK((f.insp->addable(a) == std::vector<std::string>{"SpriteRenderer"}));
    CHECK(f.insp->addComponent(a, "SpriteRenderer") && !f.insp->addComponent(a, "SpriteRenderer"));
    CHECK(f.world.get<render::SpriteRenderer>(f.doc->entity(a))->size.x == 1);   // defaults
    const size_t n = f.stack->size();
    for (int i = 0; i <= 20; ++i) f.insp->setField(a, "Transform2D", "rotation", std::to_string(i * 4), 77);
    f.stack->seal();
    CHECK(f.stack->size() == n + 1);
    CHECK(near(f.world.get<Transform2D>(f.doc->entity(a))->rotation, radians(80)));
    f.stack->undo();
    CHECK(f.world.get<Transform2D>(f.doc->entity(a))->rotation == 0);
    CHECK(f.insp->removeComponent(a, "SpriteRenderer") && !f.insp->removeComponent(a, "Transform2D"));
    CHECK(!f.world.has<render::SpriteRenderer>(f.doc->entity(a)));
    f.stack->undo();
    CHECK(f.world.has<render::SpriteRenderer>(f.doc->entity(a)));
}

TEST(inspector_text_roundtrip_all_types) {
    struct All { float f; int32_t i; uint32_t u; vec2 v2; ecs::Entity e; vec4 v4; int16_t s; uint16_t us; Name n; float ang; uint32_t col; };
    const serial::Field fields[] = {
        MGE_FIELD(All, f, F32), MGE_FIELD(All, i, I32), MGE_FIELD(All, u, U32), MGE_FIELD(All, v2, Vec2),
        MGE_FIELD(All, e, Entity), MGE_FIELD(All, v4, Vec4), MGE_FIELD(All, s, I16), MGE_FIELD(All, us, U16),
        MGE_FIELD(All, n, Str32), MGE_FIELD_HINT(All, ang, F32, Angle), MGE_FIELD_HINT(All, col, U32, Color)};
    const char* texts[] = {"0.1", "-7", "4000000000", "1 -2", "42", "0 0.25 0.5 1", "-32768", "65535", "Hello", "45", "#11223344"};
    All a{};
    for (size_t k = 0; k < 11; ++k) {
        CHECK(Inspector::parse(fields[k], texts[k], reinterpret_cast<uint8_t*>(&a)));
        CHECK(Inspector::format(fields[k], reinterpret_cast<const uint8_t*>(&a)) == texts[k]);
    }
    CHECK(near(a.ang, kPi / 4) && a.col == 0x44332211u && a.e.index == 42);
    CHECK(Inspector::parse(fields[4], "0", reinterpret_cast<uint8_t*>(&a)) && a.e.index == UINT32_MAX);
}
