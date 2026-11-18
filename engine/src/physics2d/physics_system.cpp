#include "mge/physics2d/physics_system.h"

namespace mge::physics2d {

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
