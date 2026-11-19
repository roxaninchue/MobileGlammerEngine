#include "test_main.h"
#include "mge/editor/editor_session.h"
#include "mge/editor/editor_viewport.h"
#include "mge/editor/gizmo.h"
#include <cmath>
using namespace mge;
using namespace mge::editor;
using scene2d::Transform2D;
using input::TouchAction;
using input::TouchEvent;

namespace {
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
constexpr int64_t kMs = 1'000'000;

struct Fx {
    EditorSession s;
    render::ScreenLayout view = render::ScreenLayout::compute({}, 1600, 900);   // 100 px per unit, origin at (800, 450)
    Uid box = kNoUid;
    Fx() {
        s.newScene(false);
        s.handle("create 0 Box");
        box = s.selection().primary();
        s.update();
    }
    Transform2D& t(Uid u) { return *s.world().get<Transform2D>(s.doc().entity(u)); }
    vec2 worldPos(Uid u) { s.update(); return s.world().get<scene2d::WorldTransform2D>(s.doc().entity(u))->m.translation(); }
    void dragGizmo(vec2 from, vec2 to) {
        CHECK(s.gizmo().begin(s, view, from));
        s.gizmo().drag(s, view, (from + to) * 0.5f);
        s.gizmo().drag(s, view, to);
        s.gizmo().end(s);
    }
};
}  // namespace

TEST(gizmo_hit_test) {
    Fx f;
    Gizmo& g = f.s.gizmo();
    const scene2d::Affine2 m = f.s.hierarchy().worldMatrix(f.box);
    CHECK(near(f.view.pixelsPerUnit, 100));
    CHECK(g.hitTest(f.view, m, {800, 450}) == GizmoHandle::MoveFree);
    CHECK(g.hitTest(f.view, m, {880, 450}) == GizmoHandle::MoveX);
    CHECK(g.hitTest(f.view, m, {845, 455}) == GizmoHandle::MoveX);   // on the shaft
    CHECK(g.hitTest(f.view, m, {800, 370}) == GizmoHandle::MoveY);   // world +Y is screen up
    CHECK(g.hitTest(f.view, m, {1000, 450}) == GizmoHandle::None);
    g.mode = GizmoMode::Rotate;
    CHECK(g.hitTest(f.view, m, {870, 450}) == GizmoHandle::Rotate);
    CHECK(g.hitTest(f.view, m, {800, 520}) == GizmoHandle::Rotate);
    CHECK(g.hitTest(f.view, m, {840, 450}) == GizmoHandle::None);
    g.mode = GizmoMode::Scale;
    CHECK(g.hitTest(f.view, m, {880, 450}) == GizmoHandle::ScaleX);
    CHECK(g.hitTest(f.view, m, {845, 455}) == GizmoHandle::None);    // scale has no shaft hit
    CHECK(g.hitTest(f.view, m, {800, 450}) == GizmoHandle::ScaleUniform);
    g.density = 2.0f;   // handles are in dp: twice as far on a 2x screen
    CHECK(g.hitTest(f.view, m, {960, 450}) == GizmoHandle::ScaleX);
    CHECK(Gizmo::snapTo(0.37f, 0.5f) == 0.5f && Gizmo::snapTo(-0.26f, 0.5f) == -0.5f && Gizmo::snapTo(3, 0) == 3);
}

TEST(gizmo_move_axis_and_undo) {
    Fx f;
    f.dragGizmo({880, 450}, {980, 470});   // X arrow: the 20 px of vertical motion is ignored
    CHECK(near(f.t(f.box).position.x, 1) && near(f.t(f.box).position.y, 0));
    CHECK(f.s.stack().size() == 2 && f.s.stack().undoLabel() == "Move Box");   // create + one move step
    f.s.stack().undo();
    CHECK(near(f.t(f.box).position.x, 0));
    f.s.stack().redo();
    CHECK(near(f.t(f.box).position.x, 1));
    // Free move with grid snapping.
    f.s.handle("snap 1 0.5");
    f.s.update();
    const vec2 o = f.view.worldToScreen({1, 0});
    f.dragGizmo(o, o + vec2{37, -63});   // +0.37, +0.63 world -> snapped to 1.5, 0.5
    CHECK(near(f.t(f.box).position.x, 1.5f) && near(f.t(f.box).position.y, 0.5f));
    // A tap-sized drag that does not move records nothing.
    const size_t n = f.s.stack().size();
    f.s.update();
    const vec2 o2 = f.view.worldToScreen({1.5f, 0.5f});
    f.dragGizmo(o2, o2 + vec2{2, 0});
    CHECK(f.s.stack().size() == n);
}

TEST(gizmo_move_child_of_rotated_scaled_parent) {
    Fx f;
    f.s.handle("set " + std::to_string(f.box) + " Transform2D rotation 90");
    f.s.handle("set " + std::to_string(f.box) + " Transform2D scale 2 2");
    f.s.handle("create " + std::to_string(f.box) + " Child");
    const Uid child = f.s.selection().primary();
    f.s.handle("set " + std::to_string(child) + " Transform2D position 1 0");
    const vec2 before = f.worldPos(child);
    CHECK(near(before.x, 0) && near(before.y, 2));
    const vec2 o = f.view.worldToScreen(before);
    f.dragGizmo(o, o + vec2{150, 0});   // free move 1.5 world units right
    const vec2 after = f.worldPos(child);
    CHECK(near(after.x, 1.5f) && near(after.y, 2));
    CHECK(near(f.t(child).position.x, 1) && near(f.t(child).position.y, -0.75f));   // in parent space
}

TEST(gizmo_rotate_with_snap) {
    Fx f;
    f.s.handle("gizmo rotate");
    f.dragGizmo({870, 450}, {800, 380});   // quarter turn counter-clockwise
    CHECK(near(degrees(f.t(f.box).rotation), 90, 0.05f));
    CHECK(f.s.stack().undoLabel() == "Rotate Box");
    f.s.stack().undo();
    f.s.handle("snap 1 0.5 15");
    f.s.update();
    const float a = radians(50);
    f.dragGizmo({870, 450}, {800 + 70 * std::cos(a), 450 - 70 * std::sin(a)});
    CHECK(near(degrees(f.t(f.box).rotation), 45, 0.05f));
    // Past half a turn: angles accumulate instead of wrapping.
    f.s.stack().undo();
    f.s.handle("snap 0");
    f.s.update();
    CHECK(f.s.gizmo().begin(f.s, f.view, {870, 450}));
    for (int deg = 30; deg <= 270; deg += 30)
        f.s.gizmo().drag(f.s, f.view, {800 + 70 * std::cos(radians(float(deg))), 450 - 70 * std::sin(radians(float(deg)))});
    f.s.gizmo().end(f.s);
    CHECK(near(degrees(f.t(f.box).rotation), 270, 0.05f));
}

TEST(gizmo_scale_axis_uniform_snap) {
    Fx f;
    f.s.handle("gizmo scale");
    f.dragGizmo({880, 450}, {960, 450});   // X box from 80 px to 160 px: x2
    CHECK(near(f.t(f.box).scale.x, 2) && near(f.t(f.box).scale.y, 1));
    f.dragGizmo({812, 450}, {824, 450});   // uniform from 12 px to 24 px: x2 both
    CHECK(near(f.t(f.box).scale.x, 4) && near(f.t(f.box).scale.y, 2));
    f.s.handle("snap 1 0.5 15 0.25");
    f.dragGizmo({800, 370}, {800, 330});   // Y box: 80 -> 120 px = x1.5 -> 3.0 (on the 0.25 grid)
    CHECK(near(f.t(f.box).scale.y, 3));
    f.dragGizmo({880, 450}, {800, 450});   // collapsing to zero is clamped to one step
    CHECK(near(f.t(f.box).scale.x, 0.25f));
}

TEST(gizmo_multi_select_cancel_and_lock) {
    Fx f;
    f.s.handle("create 0 Other");
    const Uid other = f.s.selection().primary();
    f.s.handle("set " + std::to_string(other) + " Transform2D position 3 0");
    f.s.handle("select " + std::to_string(f.box));
    f.s.handle("toggle " + std::to_string(other));   // primary is now Other
    f.s.update();
    const vec2 o = f.view.worldToScreen({3, 0});
    f.dragGizmo(o, o + vec2{0, -100});
    CHECK(near(f.t(f.box).position.y, 1) && near(f.t(other).position.y, 1));
    CHECK(f.s.stack().undoLabel() == "Move 2");
    f.s.stack().undo();
    CHECK(near(f.t(f.box).position.y, 0) && near(f.t(other).position.y, 0));
    // Cancel puts everything back and records nothing.
    const size_t n = f.s.stack().size();
    CHECK(f.s.gizmo().begin(f.s, f.view, o));
    f.s.gizmo().drag(f.s, f.view, o + vec2{100, 0});
    CHECK(near(f.t(other).position.x, 4));
    f.s.gizmo().cancel(f.s);
    CHECK(near(f.t(other).position.x, 3) && f.s.stack().size() == n && !f.s.gizmo().active());
    f.s.stack().lock(true);
    CHECK(!f.s.gizmo().begin(f.s, f.view, o));
}

TEST(gizmo_through_viewport_touches) {
    Fx f;
    EditorViewport vp;
    std::vector<TouchEvent> ev;
    vp.frame(f.s, ev, 0, 0.016f, 1600, 900);
    // Drag the X arrow tip 100 px right: the box moves, the camera does not.
    ev = {{TouchAction::Down, 0, {880, 450}, 10 * kMs}, {TouchAction::Move, 0, {930, 450}, 30 * kMs},
          {TouchAction::Move, 0, {980, 450}, 50 * kMs}};
    vp.frame(f.s, ev, 50 * kMs, 0.016f, 1600, 900);
    CHECK(near(f.t(f.box).position.x, 1) && near(vp.offset().x, 0));
    ev = {{TouchAction::Up, 0, {980, 450}, 70 * kMs}};
    vp.frame(f.s, ev, 70 * kMs, 0.016f, 1600, 900);
    CHECK(f.s.stack().undoLabel() == "Move Box" && f.s.stack().size() == 2);
    // A second finger mid-drag cancels the edit (and starts a pinch).
    f.s.update();
    ev = {{TouchAction::Down, 0, {980, 450}, 1000 * kMs}, {TouchAction::Move, 0, {1080, 450}, 1020 * kMs}};
    vp.frame(f.s, ev, 1020 * kMs, 0.016f, 1600, 900);
    CHECK(near(f.t(f.box).position.x, 2));
    ev = {{TouchAction::Down, 1, {500, 500}, 1040 * kMs}};
    vp.frame(f.s, ev, 1040 * kMs, 0.016f, 1600, 900);
    CHECK(near(f.t(f.box).position.x, 1) && f.s.stack().size() == 2);
    // Dragging empty space still pans.
    ev = {{TouchAction::Up, 0, {1080, 450}, 1100 * kMs}, {TouchAction::Up, 1, {500, 500}, 1100 * kMs},
          {TouchAction::Down, 0, {200, 200}, 3000 * kMs}, {TouchAction::Move, 0, {300, 200}, 3050 * kMs}};
    vp.frame(f.s, ev, 3050 * kMs, 0.016f, 1600, 900);
    CHECK(near(vp.offset().x, -1) && near(f.t(f.box).position.x, 1));
}

TEST(gizmo_protocol_and_state) {
    Fx f;
    CHECK(f.s.handle("gizmo scale") && f.s.handle("space local") && f.s.handle("snap 1 0.25 10 0.5"));
    const std::string js = f.s.stateJson();
    CHECK(js.find("\"gizmo\":{\"mode\":\"scale\",\"local\":true,\"snap\":true,\"grid\":0.25,\"angle\":10,\"scaleStep\":0.5}") !=
          std::string::npos);
    CHECK(!f.s.handle("gizmo shear") && !f.s.handle("space up") && !f.s.handle("snap 1 -2") && !f.s.handle("snap x"));
    CHECK(f.s.gizmo().snap.grid == 0.25f);
}
