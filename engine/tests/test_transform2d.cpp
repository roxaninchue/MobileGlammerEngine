#include "test_main.h"
#include "mge/transform2d.h"
#include <cmath>
using namespace mge;
using namespace mge::scene2d;
using ecs::Entity;

static bool near(vec2 a, vec2 b) { return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f; }

TEST(affine2_trs_inverse) {
    Affine2 m = Affine2::trs({10, 5}, radians(90), {2, 2});
    CHECK(near(m.apply({1, 0}), {10, 7}));          // x axis rotates to +y, scaled by 2
    CHECK(near(m.inverse().apply(m.apply({3, -4})), {3, -4}));
}

TEST(transform2d_hierarchy) {
    ecs::World w;
    TransformSystem2D ts;
    Entity root = w.create(), child = w.create(), grand = w.create();
    ts.attach(w, root, {100, 0}, radians(90));
    ts.attach(w, child, {10, 0});
    ts.attach(w, grand, {0, 5}, 0, {2, 2});
    CHECK(ts.setParent(w, child, root));
    CHECK(ts.setParent(w, grand, child));
    ts.update(w);
    CHECK(near(w.get<WorldTransform2D>(child)->m.translation(), {100, 10}));
    CHECK(near(w.get<WorldTransform2D>(grand)->m.translation(), {95, 10}));
    CHECK(!ts.setParent(w, root, grand));            // cycle rejected
    CHECK(ts.parentOf(w, root) == Entity{});
}

TEST(transform2d_dirty_propagation) {
    ecs::World w;
    TransformSystem2D ts;
    Entity root = w.create(), child = w.create(), other = w.create();
    ts.attach(w, root); ts.attach(w, child, {1, 0}); ts.attach(w, other);
    ts.setParent(w, child, root);
    ts.update(w);
    CHECK(ts.lastRecomputed() == 3);
    ts.update(w);
    CHECK(ts.lastRecomputed() == 0);                 // nothing moved, nothing recomputed
    w.get<Transform2D>(root)->setPosition({5, 5});
    ts.update(w);
    CHECK(ts.lastRecomputed() == 2);                 // root + its child, not `other`
    CHECK(near(w.get<WorldTransform2D>(child)->m.translation(), {6, 5}));
    CHECK(w.get<WorldTransform2D>(other)->changedFrame == 1);
}

TEST(transform2d_orphan_and_reparent) {
    ecs::World w;
    TransformSystem2D ts;
    Entity a = w.create(), b = w.create(), c = w.create();
    ts.attach(w, a, {10, 0}); ts.attach(w, b, {20, 0}); ts.attach(w, c, {1, 1});
    ts.setParent(w, c, a);
    ts.update(w);
    CHECK(near(w.get<WorldTransform2D>(c)->m.translation(), {11, 1}));
    ts.setParent(w, c, b);
    ts.update(w);
    CHECK(near(w.get<WorldTransform2D>(c)->m.translation(), {21, 1}));
    w.destroy(b);
    ts.update(w);                                    // parent gone: c becomes a root
    CHECK(!w.has<Parent>(c));
    CHECK(near(w.get<WorldTransform2D>(c)->m.translation(), {1, 1}));
}

TEST(transform2d_parallel_roots) {
    ecs::World w;
    TransformSystem2D ts;
    ts.parallelThreshold = 1000;
    for (int i = 0; i < 20000; ++i) { Entity e = w.create(); ts.attach(w, e, {float(i), 0}); }
    ts.update(w);
    CHECK(ts.lastRecomputed() == 20000);
    double sum = 0;
    w.each<WorldTransform2D>([&](Entity, WorldTransform2D& t) { sum += t.m.tx; });
    CHECK(sum == 199990000.0);
}
