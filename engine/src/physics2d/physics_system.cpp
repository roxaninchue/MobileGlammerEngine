#include "mge/physics2d/physics_system.h"

#include <cmath>
#include <vector>

namespace mge::physics2d {

void registerPhysics2D(serial::Registry& reg) {
    if (reg.find("Collider2D")) return;
    reg.add<Collider2D>("Collider2D", {MGE_FIELD_ENUM(Collider2D, bodyType, U32, "Static|Kinematic|Dynamic"),
                                       MGE_FIELD_ENUM(Collider2D, shape, U32, "Box|Circle"),
                                       MGE_FIELD(Collider2D, size, Vec2),
                                       MGE_FIELD(Collider2D, offset, Vec2),
                                       MGE_FIELD(Collider2D, density, F32),
                                       MGE_FIELD(Collider2D, friction, F32),
                                       MGE_FIELD(Collider2D, restitution, F32),
                                       MGE_FIELD(Collider2D, gravityScale, F32),
                                       MGE_FIELD_HINT(Collider2D, fixedRotation, U32, Bool),
                                       MGE_FIELD_HINT(Collider2D, sensor, U32, Bool)});
}

uint32_t PhysicsSystem2D::createFromColliders(ecs::World& w) {
    struct Item { ecs::Entity e; Collider2D c; scene2d::Affine2 m; bool child; };
    std::vector<Item> items;   // collected first: attach() adds components (structural change)
    w.each<Collider2D, scene2d::WorldTransform2D>([&](ecs::Entity e, Collider2D& c, scene2d::WorldTransform2D& wt) {
        items.push_back({e, c, wt.m, w.has<scene2d::Parent>(e)});
    });
    uint32_t made = 0;
    for (const Item& it : items) {
        const scene2d::Affine2& m = it.m;
        const float sx = std::sqrt(m.a * m.a + m.b * m.b);
        const float sy = sx > 1e-8f ? std::fabs(m.a * m.d - m.b * m.c) / sx : 0.0f;
        if (sx < 1e-6f || sy < 1e-6f) continue;   // zero-scaled: nothing to collide with
        ShapeDef sd;
        const vec2 off{it.c.offset.x * sx, it.c.offset.y * sy};
        if (it.c.shape == 1) {
            Circle circle;
            circle.center = off;
            circle.radius = max(0.005f, 0.5f * std::fabs(it.c.size.x) * max(sx, sy));
            sd.shape = Shape(circle);
        } else {
            const float hx = max(0.005f, 0.5f * std::fabs(it.c.size.x) * sx);
            const float hy = max(0.005f, 0.5f * std::fabs(it.c.size.y) * sy);
            sd.shape = Shape(makeBox(hx, hy, off, 0.0f));
        }
        sd.density = it.c.density;
        sd.friction = it.c.friction;
        sd.restitution = it.c.restitution;
        sd.isSensor = it.c.sensor != 0;
        sd.userData = packEntity(it.e);
        BodyDef bd;
        bd.type = it.c.bodyType == 0 ? BodyType::Static : it.c.bodyType == 1 ? BodyType::Kinematic : BodyType::Dynamic;
        bd.gravityScale = it.c.gravityScale;
        bd.fixedRotation = it.c.fixedRotation != 0;
        if (it.child) {
            // Bodies only drive root transforms: a child's collider is static at its world pose.
            bd.type = BodyType::Static;
            bd.position = m.translation();
            bd.angle = std::atan2(m.b, m.a);
            bd.userData = packEntity(it.e);
            const BodyId id = world_.createBody(bd);
            world_.createShape(id, sd);
        } else {
            attach(w, it.e, bd, sd);
        }
        ++made;
    }
    return made;
}

BodyId PhysicsSystem2D::attach(ecs::World& w, ecs::Entity e, BodyDef def) {
    if (!w.alive(e)) return {};
    detach(w, e);
    if (const scene2d::Transform2D* t = w.get<scene2d::Transform2D>(e)) {
        def.position = t->position;
        def.angle = t->rotation;
    } else {
        scene2d::TransformSystem2D::attach(w, e, def.position, def.angle);
    }
    def.userData = packEntity(e);
    const BodyId id = world_.createBody(def);
    w.add<RigidBody2D>(e, RigidBody2D{id, 1});
    return id;
}

BodyId PhysicsSystem2D::attach(ecs::World& w, ecs::Entity e, const BodyDef& def, const ShapeDef& shape) {
    const BodyId id = attach(w, e, def);
    if (id.valid()) world_.createShape(id, shape);
    return id;
}

void PhysicsSystem2D::detach(ecs::World& w, ecs::Entity e) {
    if (RigidBody2D* rb = w.get<RigidBody2D>(e)) {
        world_.destroyBody(rb->body);
        w.remove<RigidBody2D>(e);
    }
}

BodyId PhysicsSystem2D::bodyOf(ecs::World& w, ecs::Entity e) {
    const RigidBody2D* rb = w.get<RigidBody2D>(e);
    return rb && world_.isValid(rb->body) ? rb->body : BodyId{};
}

void PhysicsSystem2D::teleport(ecs::World& w, ecs::Entity e, vec2 position, float angle) {
    RigidBody2D* rb = w.get<RigidBody2D>(e);
    if (!rb) return;
    world_.setTransform(rb->body, position, angle);
    rb->wasAwake = 1;
    if (scene2d::Transform2D* t = w.get<scene2d::Transform2D>(e)) {
        t->setPosition(position);
        t->setRotation(angle);
    }
}

float PhysicsSystem2D::update(ecs::World& w, float frameDt) {
    // Entities destroyed since the last update take their bodies with them (their end events
    // are reported below, still naming the dead entity).
    dead_.clear();
    world_.eachBody([&](BodyId b) {
        const uint64_t u = world_.userData(b);
        if ((u & kEntityTag) && !w.alive(unpackEntity(u))) dead_.push_back(b);
    });

    const float alpha = world_.advance(frameDt);
    // Contacts ended by destroying a body are reported with this frame's events.
    for (BodyId b : dead_) world_.destroyBody(b);

    w.each<RigidBody2D, scene2d::Transform2D>([&](ecs::Entity, RigidBody2D& rb, scene2d::Transform2D& t) {
        const bool awake = world_.isAwake(rb.body);
        if (!awake && !rb.wasAwake) return;   // asleep or static and already written: free
        rb.wasAwake = awake ? 1 : 0;
        if (!world_.isValid(rb.body)) return;
        const Transform2 xf = (interpolate && awake) ? world_.interpolatedTransform(rb.body, alpha)
                                                     : world_.transform(rb.body);
        t.position = xf.p;
        t.rotation = xf.q.angle();
        t.dirty = 1;
    });

    events_.clear();
    for (const ContactBeginEvent& ev : world_.contactBeginEvents())
        events_.push_back({CollisionEventType::Begin, unpackEntity(ev.userA), unpackEntity(ev.userB), ev.a, ev.b,
                           ev.point, ev.normal, 0.0f});
    for (const ContactHitEvent& ev : world_.contactHitEvents())
        events_.push_back({CollisionEventType::Hit, unpackEntity(ev.userA), unpackEntity(ev.userB), ev.a, ev.b,
                           ev.point, ev.normal, ev.approachSpeed});
    for (const ContactEndEvent& ev : world_.contactEndEvents())
        events_.push_back({CollisionEventType::End, unpackEntity(ev.userA), unpackEntity(ev.userB), ev.a, ev.b, {}, {}, 0.0f});
    for (const SensorBeginEvent& ev : world_.sensorBeginEvents())
        events_.push_back({CollisionEventType::SensorBegin, unpackEntity(ev.userSensor), unpackEntity(ev.userVisitor),
                           ev.sensor, ev.visitor, {}, {}, 0.0f});
    for (const SensorEndEvent& ev : world_.sensorEndEvents())
        events_.push_back({CollisionEventType::SensorEnd, unpackEntity(ev.userSensor), unpackEntity(ev.userVisitor),
                           ev.sensor, ev.visitor, {}, {}, 0.0f});
    return alpha;
}

}  // namespace mge::physics2d
