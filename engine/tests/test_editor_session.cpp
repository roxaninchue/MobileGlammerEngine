#include "test_main.h"
#include "mge/editor/editor_session.h"
#include "mge/editor/editor_viewport.h"
#include <cmath>
using namespace mge;
using namespace mge::editor;
using input::TouchAction;
using input::TouchEvent;

namespace {
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
constexpr int64_t kMs = 1'000'000;
Uid byName(EditorSession& s, const char* name) {
    for (const TreeRow& r : s.hierarchy().rows(s.selection())) if (r.name == name) return r.uid;
    return kNoUid;
}
}  // namespace

TEST(editor_session_starter_and_state) {
    EditorSession s;
    s.newScene(true);
    s.update();
    CHECK(s.world().aliveCount() == 5);
    CHECK(!s.stack().canUndo() && !s.stack().isDirty());
    std::string js = s.stateJson();
    CHECK(has(js, "\"n\":\"Ground\"") && has(js, "\"n\":\"Crate (2)\"") && has(js, "\"insp\":null"));
    const Uid player = byName(s, "Player"), hat = byName(s, "Hat");
    CHECK(player && hat && s.doc().parentOf(hat) == player);
    CHECK(s.handle("expand " + std::to_string(player) + " 0"));
    CHECK(!has(s.stateJson(), "\"n\":\"Hat\""));   // collapsed under Player
    const uint64_t rev = s.revision();
    CHECK(s.handle("select " + std::to_string(hat)));
    CHECK(s.revision() > rev);
    js = s.stateJson();
    CHECK(has(js, "\"n\":\"Hat\"") && has(js, "\"name\":\"Hat\""));   // selecting expanded its parent
    CHECK(has(js, "{\"name\":\"rotation\",\"type\":\"F32\",\"hint\":\"Angle\",\"value\":\"0\"}"));
    CHECK(has(js, "\"addable\":[\"Collider2D\"]"));
    s.newScene(false);
    CHECK(s.world().aliveCount() == 0 && s.revision() > rev);
}

TEST(editor_session_protocol) {
    EditorSession s;
    s.newScene(false);
    CHECK(s.handle("create 0 Hero Ship"));
    const Uid hero = s.selection().primary();
    CHECK(s.hierarchy().nameOf(hero) == "Hero Ship");
    CHECK(s.handle("create " + std::to_string(hero) + " Gun"));
    const Uid gun = s.selection().primary();
    CHECK(s.doc().parentOf(gun) == hero);
    CHECK(s.handle("set " + std::to_string(gun) + " Transform2D position 1 2"));
    CHECK(s.handle("rename " + std::to_string(gun) + " Big \"Gun\""));
    CHECK(s.hierarchy().nameOf(gun) == "Big \"Gun\"");
    CHECK(has(s.stateJson(), "\"n\":\"Big \\\"Gun\\\"\""));
    CHECK(s.handle("reparent " + std::to_string(gun) + " 0 0"));
    CHECK(s.doc().children(kNoUid)[0] == gun);
    CHECK(s.handle("reorder " + std::to_string(gun) + " 1"));
    CHECK(s.doc().children(kNoUid)[1] == gun);
    for (int i = 1; i <= 10; ++i) CHECK(s.handle("scrub 5 " + std::to_string(gun) + " Transform2D rotation " + std::to_string(i)));
    CHECK(s.handle("seal"));
    CHECK(s.stack().undoLabel() == "Edit Transform2D.rotation");
    CHECK(s.handle("undo"));
    CHECK(s.world().get<scene2d::Transform2D>(s.doc().entity(gun))->rotation == 0);
    CHECK(s.handle("redo"));
    CHECK(s.handle("remove " + std::to_string(gun) + " SpriteRenderer"));
    CHECK(has(s.stateJson(), "\"addable\":[\"SpriteRenderer\",\"Collider2D\"]"));
    CHECK(s.handle("add " + std::to_string(gun) + " SpriteRenderer"));
    CHECK(s.handle("select " + std::to_string(hero)) && s.handle("duplicate"));
    CHECK(s.world().aliveCount() == 3 && s.selection().primary() != hero);
    CHECK(s.handle("delete") && s.world().aliveCount() == 2 && s.selection().empty());
    CHECK(s.handle("expand " + std::to_string(hero) + " 1"));
    // Rejected input.
    CHECK(!s.handle("frobnicate 1"));
    CHECK(!s.handle("select 999"));
    CHECK(!s.handle("select x"));
    CHECK(!s.handle("set 1"));
    CHECK(!s.handle("rename " + std::to_string(hero)));
    CHECK(!s.handle("reparent " + std::to_string(hero) + " " + std::to_string(hero) + " 0"));
    CHECK(!s.handle("scrub 0 1 Transform2D rotation 1"));
    CHECK(!s.handle("create 999 Orphan"));
}

TEST(editor_session_pick_and_draw) {
    EditorSession s;
    s.newScene(false);
    s.handle("create 0 Back");
    const Uid back = s.selection().primary();
    s.handle("set " + std::to_string(back) + " SpriteRenderer size 4 4");
    s.handle("create 0 Front");
    const Uid front = s.selection().primary();
    s.handle("set " + std::to_string(front) + " Transform2D position 1 0");
    s.update();
    CHECK(s.pick({1.2f, 0.1f}) == front);   // newest on top at equal layers
    s.handle("set " + std::to_string(back) + " SpriteRenderer layer 5");
    s.update();
    CHECK(s.pick({1.2f, 0.1f}) == back);
    CHECK(s.pick({-1.9f, 1.9f}) == back && s.pick({9, 9}) == kNoUid);
    s.handle("set " + std::to_string(back) + " SpriteRenderer visible false");
    s.update();
    CHECK(s.pick({1.2f, 0.1f}) == front);
    // Rotated entity: the pick follows the rotation.
    s.handle("set " + std::to_string(front) + " SpriteRenderer size 4 0.5");
    s.handle("set " + std::to_string(front) + " Transform2D rotation 90");
    s.update();
    CHECK(s.pick({1.0f, 1.5f}) == front && s.pick({2.5f, 0.0f}) == kNoUid);

    render::SpriteBatcher b;
    const render::ScreenLayout view = render::ScreenLayout::compute({}, 800, 450);   // 50 px per unit
    b.begin(view.camera);
    s.draw(b, view);
    b.end();
    const uint32_t withOverlay = b.stats().submitted;
    b.begin(view.camera);
    s.draw(b, view, false);
    b.end();
    CHECK(b.stats().submitted == 1);              // only the visible sprite
    CHECK(withOverlay >= 1 + 17 + 10 + 4 + 5);    // grid lines + selection outline + gizmo
}

TEST(editor_viewport_tap_select_pan_zoom) {
    EditorSession s;
    s.newScene(false);
    s.handle("create 0 Box");
    const Uid box = s.selection().primary();
    s.handle("select 0");
    s.update();
    EditorViewport vp;   // design area 16 x 9 world units
    std::vector<TouchEvent> ev;
    vp.frame(s, ev, 0, 0.016f, 1600, 900);
    CHECK(std::fabs(vp.layout().pixelsPerUnit - 100) < 1e-3f);
    CHECK(!s.selection().contains(box));
    // Tap the centre of the screen (the box at the origin).
    ev = {{TouchAction::Down, 0, {800, 450}, 10 * kMs}, {TouchAction::Up, 0, {800, 450}, 60 * kMs}};
    vp.frame(s, ev, 60 * kMs, 0.016f, 1600, 900);
    CHECK(s.selection().primary() == box && vp.needsRedraw());
    // Tap empty space clears.
    ev = {{TouchAction::Down, 0, {100, 100}, 1000 * kMs}, {TouchAction::Up, 0, {100, 100}, 1050 * kMs}};
    vp.frame(s, ev, 1050 * kMs, 0.016f, 1600, 900);
    CHECK(s.selection().empty());
    // Drag 200 px right: the camera moves 2 units left (content follows the finger).
    ev = {{TouchAction::Down, 0, {800, 450}, 2000 * kMs}, {TouchAction::Move, 0, {900, 450}, 2100 * kMs},
          {TouchAction::Move, 0, {1000, 450}, 2200 * kMs}};
    vp.frame(s, ev, 2200 * kMs, 0.016f, 1600, 900);
    CHECK(std::fabs(vp.offset().x + 2) < 1e-3f && std::fabs(vp.offset().y) < 1e-3f);
    ev = {{TouchAction::Up, 0, {1000, 450}, 4000 * kMs}};   // held still for long: no fling
    vp.frame(s, ev, 4000 * kMs, 0.016f, 1600, 900);
    vp.frame(s, {}, 4016 * kMs, 0.016f, 1600, 900);
    CHECK(!vp.needsRedraw());   // idle: the host can stop drawing
    // Pinch out 2x around the screen centre: zoom doubles, centre stays.
    const vec2 centreBefore = vp.screenToWorld({800, 450});
    ev = {{TouchAction::Down, 0, {700, 450}, 5000 * kMs}, {TouchAction::Down, 1, {900, 450}, 5000 * kMs},
          {TouchAction::Move, 0, {600, 450}, 5050 * kMs}, {TouchAction::Move, 1, {1000, 450}, 5050 * kMs}};
    vp.frame(s, ev, 5050 * kMs, 0.016f, 1600, 900);
    CHECK(std::fabs(vp.zoom() - 2) < 1e-3f);
    const vec2 centreAfter = vp.screenToWorld({800, 450});
    CHECK(std::fabs(centreBefore.x - centreAfter.x) < 1e-3f && std::fabs(centreBefore.y - centreAfter.y) < 1e-3f);
}
