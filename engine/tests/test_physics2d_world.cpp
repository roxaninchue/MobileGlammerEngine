#include "test_main.h"
#include "mge/physics2d/physics_system.h"
#include "mge/physics2d/world.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace mge::physics2d;
using mge::kPi;
using mge::vec2;

namespace {

bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }

BodyId addGround(World& w, float y = 0.0f, float halfWidth = 50.0f, float friction = 0.6f) {
    BodyDef bd;
    bd.type = BodyType::Static;
    bd.position = {0, y - 0.5f};
    const BodyId g = w.createBody(bd);
    ShapeDef sd;
    sd.shape = makeBox(halfWidth, 0.5f);
    sd.friction = friction;
    w.createShape(g, sd);
    return g;
}

BodyId addBox(World& w, vec2 p, float hx = 0.5f, float hy = 0.5f, float angle = 0.0f, float friction = 0.6f) {
    BodyDef bd;
    bd.position = p;
    bd.angle = angle;
    const BodyId b = w.createBody(bd);
    ShapeDef sd;
    sd.shape = makeBox(hx, hy);
    sd.friction = friction;
    w.createShape(b, sd);
    return b;
}

BodyId addBall(World& w, vec2 p, float r, float restitution = 0.0f) {
    BodyDef bd;
    bd.position = p;
    const BodyId b = w.createBody(bd);
    ShapeDef sd;
    sd.shape = Circle{{0, 0}, r};
    sd.restitution = restitution;
    w.createShape(b, sd);
    return b;
}

void run(World& w, float seconds) {
    const int steps = int(seconds / w.fixedDt() + 0.5f);
    for (int i = 0; i < steps; ++i) w.step(w.fixedDt());
}

}  // namespace

TEST(physics2d_world_free_fall_matches_integrator) {
    World w;
    const BodyId b = addBall(w, {0, 100}, 0.5f);
    run(w, 1.0f);
    // Symplectic Euler: v = g t, y = y0 + g h^2 n(n+1)/2.
    const float h = w.fixedDt();
    const int n = 60;
    CHECK(near(w.linearVelocity(b).y, -10.0f, 1e-3f));
    CHECK(near(w.position(b).y, 100.0f - 10.0f * h * h * float(n * (n + 1)) * 0.5f, 1e-3f));
    CHECK(near(w.mass(b), kPi * 0.25f, 1e-4f));
}

TEST(physics2d_world_box_comes_to_rest_and_sleeps) {
    World w;
    addGround(w);
    const BodyId b = addBox(w, {0, 3}, 0.5f, 0.5f, 0.3f);
    run(w, 3.0f);
    CHECK(near(w.position(b).y, 0.5f, 2.0f * kLinearSlop));
    // Settled flat on a face (any multiple of 90 degrees).
    const float a = std::fmod(std::fabs(w.angle(b)), 0.5f * kPi);
    CHECK(a < 0.01f || a > 0.5f * kPi - 0.01f);
    CHECK(!w.isAwake(b));
    CHECK(w.stats().awakeBodies == 0);
}

TEST(physics2d_world_stack_is_stable) {
    World w;
    addGround(w);
    BodyId boxes[10];
    for (int i = 0; i < 10; ++i) boxes[i] = addBox(w, {0, 0.5f + 1.0f * float(i)});
    run(w, 5.0f);
    for (int i = 0; i < 10; ++i) {
        const vec2 p = w.position(boxes[i]);
        CHECK(std::fabs(p.x) < 0.02f);
        CHECK(near(p.y, 0.5f + float(i), 0.03f * float(i + 1)));
    }
    CHECK(!w.isAwake(boxes[9]));
}

TEST(physics2d_world_pyramid_settles) {
    World w;
    addGround(w);
    std::vector<BodyId> boxes;
    const int rows = 12;
    for (int r = 0; r < rows; ++r)
        for (int i = 0; i < rows - r; ++i)
            boxes.push_back(addBox(w, {float(i) - float(rows - r - 1) * 0.5f, 0.5f + float(r)}));
    const vec2 top0 = w.position(boxes.back());
    run(w, 6.0f);
    const vec2 top = w.position(boxes.back());
    CHECK(std::fabs(top.x - top0.x) < 0.05f);
    CHECK(std::fabs(top.y - top0.y) < 0.12f);   // compressed a little, never collapsed
    int awake = 0;
    for (BodyId b : boxes) awake += w.isAwake(b) ? 1 : 0;
    CHECK(awake == 0);
}

TEST(physics2d_world_restitution_bounce) {
    World w;
    addGround(w);
    const BodyId ball = addBall(w, {0, 5.5f}, 0.5f, 0.8f);
    float peakAfterBounce = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 240; ++i) {
        w.step(w.fixedDt());
        const float vy = w.linearVelocity(ball).y;
        if (!bounced && vy > 0.0f) bounced = true;
        if (bounced) peakAfterBounce = mge::max(peakAfterBounce, w.position(ball).y);
        if (bounced && vy < 0.0f) break;
    }
    CHECK(bounced);
    // Dropped from 5 m of clearance: e = 0.8 gives about 0.64 * 5 = 3.2 m.
    const float rebound = peakAfterBounce - 0.5f;
    CHECK(rebound > 2.7f && rebound < 3.7f);
}

TEST(physics2d_world_no_bounce_below_threshold) {
    World w;
    addGround(w);
    const BodyId ball = addBall(w, {0, 0.52f}, 0.5f, 1.0f);   // tiny drop: slower than 1 m/s
    run(w, 1.0f);
    CHECK(near(w.position(ball).y, 0.5f, 2.0f * kLinearSlop));
}

TEST(physics2d_world_friction_on_slope) {
    // A box on a 20-degree ramp: friction 0.6 > tan(20) holds it, 0.1 lets it slide.
    for (int pass = 0; pass < 2; ++pass) {
        const float friction = pass == 0 ? 0.6f : 0.1f;
        World w;
        const float slope = 20.0f * kPi / 180.0f;
        BodyDef rd;
        rd.type = BodyType::Static;
        rd.angle = slope;
        const BodyId ramp = w.createBody(rd);
        ShapeDef rs;
        rs.shape = makeBox(20.0f, 0.5f);
        rs.friction = friction;
        w.createShape(ramp, rs);
        const vec2 up{-std::sin(slope), std::cos(slope)};
        const BodyId box = addBox(w, up * 1.0f, 0.5f, 0.5f, slope, friction);
        const vec2 start = w.position(box);
        run(w, 2.0f);
        const float moved = length(w.position(box) - start);
        if (pass == 0) CHECK(moved < 0.05f);
        else CHECK(moved > 2.0f);
    }
}

TEST(physics2d_world_contact_events) {
    World w;
    addGround(w);
    const BodyId ball = addBall(w, {0, 3}, 0.5f);
    int begins = 0, hits = 0, ends = 0;
    float hitSpeed = 0.0f;
    for (int i = 0; i < 120; ++i) {
        w.step(w.fixedDt());
        begins += int(w.contactBeginEvents().size());
        ends += int(w.contactEndEvents().size());
        for (const ContactHitEvent& h : w.contactHitEvents()) { ++hits; hitSpeed = h.approachSpeed; }
    }
    CHECK(begins == 1 && ends == 0 && hits == 1);
    CHECK(hitSpeed > 6.5f && hitSpeed < 7.6f);   // ~sqrt(2 g 2.5) = 7.07
    // Lifting it off ends the contact.
    w.setTransform(ball, {0, 10}, 0.0f);
    w.step(w.fixedDt());
    CHECK(w.contactEndEvents().size() == 1);
}

TEST(physics2d_world_sensor_events) {
    World w;
    BodyDef sd;
    sd.type = BodyType::Static;
    sd.position = {0, 5};
    const BodyId zone = w.createBody(sd);
    ShapeDef zs;
    zs.shape = makeBox(2.0f, 0.5f);
    zs.isSensor = true;
    const ShapeId sensor = w.createShape(zone, zs);
    const BodyId ball = addBall(w, {0, 8}, 0.25f);
    int begin = 0, end = 0;
    for (int i = 0; i < 90; ++i) {
        w.step(w.fixedDt());
        for (const SensorBeginEvent& e : w.sensorBeginEvents()) { ++begin; CHECK(e.sensor == sensor); }
        end += int(w.sensorEndEvents().size());
    }
    CHECK(begin == 1 && end == 1);
    CHECK(w.position(ball).y < 4.0f);   // fell straight through
    CHECK(w.mass(zone) == 0.0f);
}

TEST(physics2d_world_filters) {
    World w;
    addGround(w);
    BodyDef bd;
    bd.position = {0, 2};
    const BodyId ghost = w.createBody(bd);
    ShapeDef sd;
    sd.shape = makeBox(0.5f, 0.5f);
    sd.filter.categoryBits = 2;
    sd.filter.maskBits = ~1u;   // ignores the ground's category 1
    const ShapeId gs = w.createShape(ghost, sd);
    run(w, 1.0f);
    CHECK(w.position(ghost).y < -1.0f);
    // Re-enabling the mask makes it collide again (teleported back above the ground).
    Filter f = sd.filter;
    f.maskBits = 0xFFFFFFFFu;
    w.setFilter(gs, f);
    w.setTransform(ghost, {0, 2}, 0.0f);
    w.setLinearVelocity(ghost, {});
    run(w, 2.0f);
    CHECK(near(w.position(ghost).y, 0.5f, 0.02f));
    // Overlapping boxes push apart, unless they share a negative group.
    for (int pass = 0; pass < 2; ++pass) {
        World w2;
        w2.setGravity({0, 0});
        BodyId boxes[2];
        for (int i = 0; i < 2; ++i) {
            BodyDef d;
            d.position = {0, 0.6f * float(i)};
            boxes[i] = w2.createBody(d);
            ShapeDef s2;
            s2.shape = makeBox(0.5f, 0.5f);
            s2.filter.groupIndex = pass == 0 ? 0 : -1;
            w2.createShape(boxes[i], s2);
        }
        run(w2, 1.0f);
        const float gap = w2.position(boxes[1]).y - w2.position(boxes[0]).y;
        if (pass == 0) CHECK(gap > 0.95f);
        else CHECK(near(gap, 0.6f, 1e-4f));
    }
}

TEST(physics2d_world_wake_on_impact_and_on_removal) {
    World w;
    addGround(w);
    BodyId stack[3];
    for (int i = 0; i < 3; ++i) stack[i] = addBox(w, {0, 0.5f + float(i)});
    run(w, 3.0f);
    CHECK(!w.isAwake(stack[0]) && !w.isAwake(stack[2]));
    const int steps0 = 0;
    (void)steps0;
    // A ball dropped on top wakes the whole island.
    addBall(w, {0, 6}, 0.3f);
    run(w, 0.9f);
    CHECK(w.isAwake(stack[0]));
    run(w, 4.0f);
    CHECK(!w.isAwake(stack[0]));
    // Removing the bottom box wakes the rest, which fall.
    const float y2 = w.position(stack[2]).y;
    w.destroyBody(stack[0]);
    CHECK(!w.isValid(stack[0]));
    CHECK(w.isAwake(stack[1]));
    run(w, 2.0f);
    CHECK(w.position(stack[2]).y < y2 - 0.8f);
}

TEST(physics2d_world_kinematic_platform_carries_box) {
    World w;
    BodyDef kd;
    kd.type = BodyType::Kinematic;
    kd.linearVelocity = {1.0f, 0.0f};
    const BodyId platform = w.createBody(kd);
    ShapeDef ps;
    ps.shape = makeBox(3.0f, 0.25f);
    w.createShape(platform, ps);
    const BodyId box = addBox(w, {0, 0.75f});
    run(w, 2.0f);
    CHECK(near(w.position(platform).x, 2.0f, 1e-3f));
    CHECK(near(w.position(box).x, 2.0f, 0.1f));   // friction dragged it along
    CHECK(near(w.position(box).y, 0.75f, 0.02f));
    CHECK(w.isAwake(box));   // a moving platform keeps its island awake
}

TEST(physics2d_world_fixed_timestep_and_interpolation) {
    World w;
    const BodyId b = addBall(w, {0, 0}, 0.5f);
    w.setGravity({0, 0});
    w.setLinearVelocity(b, {6.0f, 0});
    float alpha = w.advance(0.5f / 60.0f);   // half a step: nothing runs yet
    CHECK(w.stats().stepsLastAdvance == 0 && near(alpha, 0.5f, 1e-4f));
    alpha = w.advance(1.0f / 60.0f);         // 1.5 steps banked -> one step, alpha 0.5
    CHECK(w.stats().stepsLastAdvance == 1 && near(alpha, 0.5f, 1e-3f));
    CHECK(near(w.position(b).x, 0.1f, 1e-5f));
    CHECK(near(w.interpolatedTransform(b, alpha).p.x, 0.05f, 1e-4f));
    // A long hitch runs at most maxStepsPerAdvance steps and drops the rest.
    w.advance(0.2f);
    CHECK(w.stats().stepsLastAdvance == 4);
    // Frame rate does not change the result: 120 frames of 1/120 s == 60 steps.
    World a, c;
    const BodyId ba = addBall(a, {0, 50}, 0.5f), bc = addBall(c, {0, 50}, 0.5f);
    for (int i = 0; i < 60; ++i) a.advance(1.0f / 60.0f);
    for (int i = 0; i < 120; ++i) c.advance(1.0f / 120.0f);
    CHECK(near(a.position(ba).y, c.position(bc).y, 1e-4f));
}

TEST(physics2d_world_deterministic) {
    auto build = [](World& w) {
        addGround(w);
        for (int i = 0; i < 40; ++i) {
            const float x = float((i * 37) % 11) - 5.0f, y = 2.0f + float(i) * 0.6f;
            if (i % 3 == 0) addBall(w, {x, y}, 0.3f + 0.05f * float(i % 4), 0.3f);
            else addBox(w, {x, y}, 0.3f + 0.1f * float(i % 3), 0.3f, float(i) * 0.4f);
        }
    };
    World a, b;
    build(a);
    build(b);
    run(a, 4.0f);
    run(b, 4.0f);
    bool same = true;
    a.eachBody([&](BodyId id) {
        const mge::physics2d::Transform2 x = a.transform(id), y = b.transform(id);
        same &= std::memcmp(&x, &y, sizeof(x)) == 0;
    });
    CHECK(same);
}

TEST(physics2d_world_queries_and_mass) {
    World w;
    addGround(w);
    const BodyId box = addBox(w, {3, 2});
    const World::RayResult r = w.rayCastClosest({3, 10}, {3, -10});
    CHECK(r.hit && w.shapeBody(r.shape) == box && near(r.point.y, 2.5f, 1e-3f) && near(r.normal.y, 1.0f, 1e-3f));
    ShapeId hit;
    CHECK(w.testPoint({3.2f, 2.1f}, &hit) && w.shapeBody(hit) == box);
    CHECK(!w.testPoint({10, 10}));
    // Two shapes on one body: mass adds up, centre moves between them.
    BodyDef bd;
    const BodyId dumbbell = w.createBody(bd);
    ShapeDef sd;
    sd.shape = Circle{{-1, 0}, 0.5f};
    w.createShape(dumbbell, sd);
    sd.shape = Circle{{1, 0}, 0.5f};
    w.createShape(dumbbell, sd);
    const float m1 = kPi * 0.25f;
    CHECK(near(w.mass(dumbbell), 2.0f * m1, 1e-4f));
    CHECK(near(w.inertia(dumbbell), 2.0f * (m1 * 0.125f + m1 * 1.0f), 1e-4f));
    CHECK(near(w.worldCenter(dumbbell).x, 0.0f, 1e-5f));
    // Stale ids are rejected after destroy + reuse.
    w.destroyBody(box);
    const BodyId again = addBox(w, {0, 9});
    CHECK(again.index == box.index && !w.isValid(box) && w.isValid(again));
}

TEST(physics2d_world_ecs_bridge) {
    mge::ecs::World ecs;
    PhysicsSystem2D phys;
    const mge::ecs::Entity ground = ecs.create();
    BodyDef gd;
    gd.type = BodyType::Static;
    gd.position = {0, -0.5f};
    ShapeDef gs;
    gs.shape = makeBox(20, 0.5f);
    phys.attach(ecs, ground, gd, gs);

    const mge::ecs::Entity crate = ecs.create();
    mge::scene2d::TransformSystem2D::attach(ecs, crate, {1, 4}, 0.0f);
    ShapeDef cs;
    cs.shape = makeBox(0.5f, 0.5f);
    const BodyId body = phys.attach(ecs, crate, BodyDef{}, cs);
    CHECK(phys.entityOf(body) == crate);
    CHECK(mge::physics2d::Transform2{}.p.x == 0.0f && near(phys.world().position(body).y, 4.0f, 1e-6f));

    bool sawBegin = false, sawHit = false;
    for (int i = 0; i < 180; ++i) {
        phys.update(ecs, 1.0f / 60.0f);
        for (const CollisionEvent2D& e : phys.events()) {
            const bool pair = (e.a == ground && e.b == crate) || (e.a == crate && e.b == ground);
            if (e.type == CollisionEventType::Begin && pair) sawBegin = true;
            if (e.type == CollisionEventType::Hit && pair) sawHit = true;
        }
    }
    CHECK(sawBegin && sawHit);
    const mge::scene2d::Transform2D* t = ecs.get<mge::scene2d::Transform2D>(crate);
    CHECK(t && near(t->position.y, 0.5f, 0.02f) && near(t->position.x, 1.0f, 0.01f));

    // Destroying the ground entity removes its body and reports the contact's end, naming it.
    ecs.destroy(ground);
    phys.update(ecs, 1.0f / 60.0f);
    bool sawEnd = false;
    for (const CollisionEvent2D& e : phys.events())
        if (e.type == CollisionEventType::End && (e.a == ground || e.b == ground)) sawEnd = true;
    CHECK(sawEnd);
    CHECK(phys.world().bodyCount() == 1);
    for (int i = 0; i < 30; ++i) phys.update(ecs, 1.0f / 60.0f);
    CHECK(ecs.get<mge::scene2d::Transform2D>(crate)->position.y < 0.0f);   // falls once the floor is gone
}

TEST(physics2d_world_benchmark_smoke) {
    // 600 mixed bodies poured into a box; reports the per-step cost (not a pass/fail timing).
    World w;
    addGround(w, 0.0f, 15.0f);
    BodyDef wd;
    wd.type = BodyType::Static;
    for (float x : {-15.5f, 15.5f}) {
        wd.position = {x, 15};
        const BodyId wall = w.createBody(wd);
        ShapeDef s;
        s.shape = makeBox(0.5f, 15.0f);
        w.createShape(wall, s);
    }
    for (int i = 0; i < 600; ++i) {
        const float x = -13.0f + float(i % 26), y = 1.0f + float(i / 26) * 1.1f;
        if (i & 1) addBall(w, {x, y}, 0.4f);
        else addBox(w, {x, y}, 0.4f, 0.4f, float(i) * 0.1f);
    }
    double total = 0.0, worst = 0.0;
    const int steps = 300;
    for (int i = 0; i < steps; ++i) {
        w.step(w.fixedDt());
        total += w.stats().stepMs;
        worst = mge::max(worst, w.stats().stepMs);
    }
    std::printf("  physics2d 600 bodies: %.3f ms/step avg, %.3f ms worst, %d contacts, %d awake, %zu KiB\n",
                total / steps, worst, w.stats().contacts, w.stats().awakeBodies, w.stats().memoryBytes / 1024);
    int escaped = 0;
    w.eachBody([&](BodyId b) { if (w.position(b).y < -1.0f) ++escaped; });
    CHECK(escaped == 0);
}
