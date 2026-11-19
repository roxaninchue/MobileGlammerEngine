#include "test_main.h"
#include "mge/editor/editor_session.h"
#include <cmath>
using namespace mge;
using namespace mge::editor;
using scene2d::Transform2D;

namespace {
Uid byName(EditorSession& s, const char* name) {
    for (const TreeRow& r : s.hierarchy().rows(s.selection())) if (r.name == name) return r.uid;
    return kNoUid;
}
Transform2D& tr(EditorSession& s, Uid u) { return *s.world().get<Transform2D>(s.doc().entity(u)); }
bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
int bodies(EditorSession& s) {
    int n = 0;
    s.play().physics()->world().eachBody([&](physics2d::BodyId) { ++n; });
    return n;
}
}  // namespace

TEST(play_runs_physics_and_stop_restores_exactly) {
    EditorSession s;
    s.newScene(true);
    s.update();
    const Uid crate = byName(s, "Crate");
    s.handle("select " + std::to_string(crate));
    const std::string sceneBefore = serial::saveJson(s.world(), s.registry());
    const std::vector<uint8_t> binBefore = serial::saveBinary(s.world(), s.registry());
    const float y0 = tr(s, crate).position.y;

    CHECK(s.handle("play"));
    CHECK(s.play().playing() && s.stack().locked());
    CHECK(s.play().physics() && bodies(s) == 4);   // ground, player, 2 crates
    for (int i = 0; i < 120; ++i) { s.tick(1.0f / 60.0f); s.update(); }
    CHECK(s.play().steps() == 120 && std::fabs(s.play().time() - 2.0f) < 1e-3f);
    const float y1 = tr(s, crate).position.y;
    CHECK(y1 < y0 - 1.0f);    // fell
    CHECK(y1 > -3.2f);        // and landed on the ground instead of falling through
    CHECK(has(s.stateJson(), "\"play\":{\"state\":\"playing\",\"time\":2.00,\"steps\":120}"));

    CHECK(s.handle("stop"));
    CHECK(!s.play().active() && !s.stack().locked());
    CHECK(serial::saveJson(s.world(), s.registry()) == sceneBefore);
    CHECK(serial::saveBinary(s.world(), s.registry()) == binBefore);   // bit for bit
    CHECK(s.selection().primary() == crate && s.doc().entity(crate).valid());
    CHECK(!s.world().has<physics2d::RigidBody2D>(s.doc().entity(crate)));
    s.update();
    CHECK(std::fabs(s.world().get<scene2d::WorldTransform2D>(s.doc().entity(crate))->m.ty - y0) < 1e-6f);
}

TEST(play_locks_editing_and_keeps_history) {
    EditorSession s;
    s.newScene(true);
    const Uid crate = byName(s, "Crate");
    CHECK(s.handle("set " + std::to_string(crate) + " Transform2D position 0 3"));
    CHECK(s.stack().size() == 1 && s.stack().isDirty());
    CHECK(s.handle("play"));
    // Edits, undo and structural changes are refused while playing; selection still works.
    CHECK(!s.handle("set " + std::to_string(crate) + " Transform2D position 5 5"));
    CHECK(!s.handle("undo") && !s.handle("create 0 X") && !s.handle("delete"));
    CHECK(s.handle("select " + std::to_string(crate)));
    CHECK(!s.handle("play"));   // already playing
    CHECK(has(s.stateJson(), "\"locked\":true"));
    CHECK(s.handle("stop") && !s.handle("stop"));
    CHECK(s.stack().size() == 1 && s.stack().canUndo());
    CHECK(s.handle("undo"));   // the pre-play edit is still undoable
    CHECK(tr(s, crate).position.y == 0.5f);
}

TEST(play_pause_step) {
    EditorSession s;
    s.newScene(true);
    const Uid crate = byName(s, "Crate");
    CHECK(!s.handle("pause") && !s.handle("step"));   // nothing to pause while editing
    CHECK(s.handle("play") && s.handle("pause"));
    CHECK(s.play().state() == PlayState::Paused);
    const float y0 = tr(s, crate).position.y;
    CHECK(!s.tick(0.5f) && tr(s, crate).position.y == y0);   // paused: time stands still
    CHECK(s.handle("step"));
    CHECK(s.play().steps() == 1 && tr(s, crate).position.y < y0);
    for (int i = 0; i < 9; ++i) s.handle("step");
    CHECK(s.play().steps() == 10);
    CHECK(s.handle("play") && s.play().playing());   // resume
    s.tick(1.0f / 60.0f);
    CHECK(s.play().steps() == 11);
    CHECK(s.handle("stop") && tr(s, crate).position.y == 0.5f);
}

TEST(play_colliders_scale_children_and_shapes) {
    EditorSession s;
    s.newScene(false);
    s.handle("create 0 Floor");
    const Uid floor = s.selection().primary();
    s.handle("set " + std::to_string(floor) + " Transform2D position 0 -2");
    s.handle("set " + std::to_string(floor) + " Transform2D scale 10 1");
    CHECK(s.handle("add " + std::to_string(floor) + " Collider2D"));
    CHECK(s.handle("set " + std::to_string(floor) + " Collider2D bodyType static"));   // names are case-insensitive
    CHECK(!s.handle("set " + std::to_string(floor) + " Collider2D bodyType Floating"));
    CHECK(!s.handle("set " + std::to_string(floor) + " Collider2D bodyType 3"));
    s.handle("create 0 Ball");
    const Uid ball = s.selection().primary();
    s.handle("set " + std::to_string(ball) + " Transform2D position 4.5 0");
    s.handle("add " + std::to_string(ball) + " Collider2D");
    s.handle("set " + std::to_string(ball) + " Collider2D shape Circle");
    s.handle("create " + std::to_string(floor) + " Bump");   // child collider: static at its world pose
    const Uid bump = s.selection().primary();
    s.handle("add " + std::to_string(bump) + " Collider2D");
    const std::string js = s.stateJson();
    CHECK(has(js, "{\"name\":\"bodyType\",\"type\":\"U32\",\"hint\":\"Enum\",\"value\":\"Dynamic\",\"options\":[\"Static\",\"Kinematic\",\"Dynamic\"]}"));
    CHECK(s.handle("play"));
    CHECK(bodies(s) == 3);
    for (int i = 0; i < 90; ++i) s.tick(1.0f / 60.0f);
    // The floor is 10 wide (scaled), so the ball at x = 4.5 lands on it: top at -1.5, radius 0.5.
    CHECK(std::fabs(tr(s, ball).position.y - (-1.0f)) < 0.05f);
    CHECK(tr(s, bump).position.x == 0 && tr(s, bump).position.y == 0);   // child transform untouched
    s.handle("stop");
}

TEST(play_new_scene_while_playing) {
    EditorSession s;
    s.newScene(true);
    CHECK(s.handle("play"));
    s.newScene(false);   // e.g. opening another project mid-play
    CHECK(!s.play().active() && !s.stack().locked() && s.world().aliveCount() == 0);
}
