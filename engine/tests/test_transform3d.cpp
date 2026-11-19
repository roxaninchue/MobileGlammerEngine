#include "test_main.h"
#include "mge/transform3d.h"
#include "mge/render/mesh.h"
#include "mge/serialize.h"
#include <cmath>
using namespace mge;
using namespace mge::scene3d;
using ecs::Entity;

TEST(euler_roundtrip) {
    const vec3 cases[] = {{0, 0, 0}, {0.3f, 0, 0}, {0, 1.2f, 0}, {0, 0, -2.0f}, {0.4f, -2.5f, 1.1f}, {-1.2f, 3.0f, -0.7f}};
    for (vec3 e : cases) {
        const quat q = quat::euler(e);
        const vec3 back = toEuler(q);
        CHECK(nearlyEqual(back, e, 1e-4f));
        const quat q2 = quat::euler(back);
        CHECK(std::fabs(std::fabs(dot(q, q2)) - 1.0f) < 1e-5f);
    }
    // Gimbal lock: the angles change but the rotation must not.
    const quat lock = quat::euler({kPi * 0.5f, 0.7f, 0.3f});
    const quat again = quat::euler(toEuler(lock));
    const vec3 probe{0.3f, -0.8f, 0.5f};
    CHECK(nearlyEqual(rotate(lock, probe), rotate(again, probe), 1e-4f));
}

TEST(transform3d_hierarchy_and_dirty) {
    ecs::World w;
    TransformSystem3D ts;
    Entity root = w.create(), child = w.create(), grand = w.create();
    TransformSystem3D::attach(w, root, {1, 0, 0}, quat::axisAngle({0, 1, 0}, kPi * 0.5f));
    TransformSystem3D::attach(w, child, {0, 0, 2}, {}, {2, 2, 2});
    TransformSystem3D::attach(w, grand, {1, 0, 0});
    CHECK(ts.setParent(w, child, root));
    CHECK(ts.setParent(w, grand, child));
    CHECK(!ts.setParent(w, root, grand));   // cycle
    ts.update(w);
    CHECK(ts.lastRecomputed() == 3);
    // root turns +Z into +X: child at (1,0,0) + (2,0,0); grand 1 unit along child's x (scaled 2,
    // turned to -Z) -> (3, 0, -2).
    CHECK(nearlyEqual(w.get<WorldTransform3D>(child)->position(), {3, 0, 0}));
    CHECK(nearlyEqual(w.get<WorldTransform3D>(grand)->position(), {3, 0, -2}));
    ts.update(w);
    CHECK(ts.lastRecomputed() == 0);   // static scene: nothing recomputed
    w.get<Transform3D>(root)->setPosition({0, 5, 0});
    ts.update(w);
    CHECK(ts.lastRecomputed() == 3);   // a moved parent carries its subtree
    CHECK(nearlyEqual(w.get<WorldTransform3D>(grand)->position(), {2, 5, -2}));
    // Destroying the parent detaches the child, which keeps its local transform as world.
    w.destroy(child);
    ts.update(w);
    CHECK(!w.has<Parent>(grand));
    CHECK(nearlyEqual(w.get<WorldTransform3D>(grand)->position(), {1, 0, 0}));
}

TEST(transform3d_serializes_with_vec3_and_rotation) {
    serial::Registry reg;
    serial::registerScene2D(reg);
    render::registerScene3D(reg);
    ecs::World w;
    Entity e = w.create();
    TransformSystem3D::attach(w, e, {1.5f, -2, 3}, quat::euler({0.1f, 0.2f, 0.3f}), {1, 2, 3});
    render::MeshRenderer mr;
    std::snprintf(mr.mesh, sizeof mr.mesh, "sphere");
    w.add<render::MeshRenderer>(e, mr);
    const std::string json = serial::saveJson(w, reg);
    CHECK(json.find("\"position\":[1.5,-2,3]") != std::string::npos);
    CHECK(json.find("\"version\":3") != std::string::npos);
    for (int pass = 0; pass < 2; ++pass) {
        ecs::World w2;
        bool ok;
        if (pass == 0) ok = serial::loadJson(w2, reg, json);
        else { const auto bin = serial::saveBinary(w, reg); ok = serial::loadBinary(w2, reg, bin.data(), bin.size()); }
        CHECK(ok);
        int n = 0;
        w2.each<Transform3D, render::MeshRenderer>([&](Entity e2, Transform3D& t, render::MeshRenderer& m) {
            ++n;
            CHECK(nearlyEqual(t.position, {1.5f, -2, 3}) && nearlyEqual(t.scale, {1, 2, 3}));
            CHECK(std::fabs(dot(t.rotation, quat::euler({0.1f, 0.2f, 0.3f})) - 1) < 1e-6f);
            CHECK(std::string(m.mesh) == "sphere" && m.visible == 1);
            CHECK(w2.has<WorldTransform3D>(e2));
        });
        CHECK(n == 1);
    }
}
