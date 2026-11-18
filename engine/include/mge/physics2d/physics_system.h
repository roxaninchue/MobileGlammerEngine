#pragma once
// physics2d <-> ECS bridge (M1 #19).
//  - RigidBody2D component: the entity's body in the physics World. The body's userData holds
//    the entity, so every physics event can name the entities involved.
//  - update(ecs, frameDt): advances the world on its fixed timestep, writes each awake body's
//    interpolated pose into the entity's Transform2D (and one final write when it falls asleep,
//    so sleeping costs nothing afterwards), destroys bodies whose entity died, and turns the
//    world's contact, hit and sensor events into CollisionEvent2D records keyed by entity.
//  - Bodies drive root transforms: an entity with a RigidBody2D should not have a Parent.
//    Move one by hand with teleport(), never by writing Transform2D (the next update overwrites it).
#include <cstdint>
#include <vector>
#include "mge/ecs.h"
#include "mge/physics2d/world.h"
#include "mge/transform2d.h"

namespace mge::physics2d {

struct RigidBody2D {
    BodyId body;
    uint32_t wasAwake = 1;   // internal: lets the final pose be written once when it falls asleep
};

enum class CollisionEventType : uint8_t { Begin, End, Hit, SensorBegin, SensorEnd };

// For sensor events `a` is the sensor's entity and `b` the visitor's.
struct CollisionEvent2D {
    CollisionEventType type;
    ecs::Entity a, b;        // invalid when the shape's body is not owned by an entity
    ShapeId shapeA, shapeB;
    vec2 point{}, normal{};  // Begin and Hit only; normal points from a to b
    float speed = 0.0f;      // Hit only: approach speed in m/s
};

// Body userData encoding of an entity (bit 63 marks "owned by an entity").
constexpr uint64_t kEntityTag = uint64_t(1) << 63;
inline uint64_t packEntity(ecs::Entity e) { return kEntityTag | (uint64_t(e.generation) << 32) | e.index; }
inline ecs::Entity unpackEntity(uint64_t u) {
    if (!(u & kEntityTag)) return {};
    return {uint32_t(u & 0xFFFFFFFFu), uint32_t((u >> 32) & 0x7FFFFFFFu)};
}

class PhysicsSystem2D {
public:
    explicit PhysicsSystem2D(const WorldDef& def = {}) : world_(def) {}

    World& world() { return world_; }
    const World& world() const { return world_; }

    // Creates the entity's body. The pose comes from its Transform2D when it has one (one is
    // attached otherwise, at def.position/angle). Replaces an existing body.
    BodyId attach(ecs::World& w, ecs::Entity e, BodyDef def);
    // attach + one shape, the common case.
    BodyId attach(ecs::World& w, ecs::Entity e, const BodyDef& def, const ShapeDef& shape);
    void detach(ecs::World& w, ecs::Entity e);
    BodyId bodyOf(ecs::World& w, ecs::Entity e);
    ecs::Entity entityOf(BodyId b) const { return unpackEntity(world_.userData(b)); }
    ecs::Entity entityOf(ShapeId s) const { return entityOf(world_.shapeBody(s)); }
    void teleport(ecs::World& w, ecs::Entity e, vec2 position, float angle);

    // Fixed-step advance + pose write-back + events. Returns the interpolation alpha.
    float update(ecs::World& w, float frameDt);
    const std::vector<CollisionEvent2D>& events() const { return events_; }

    // When false, poses snap to the last step instead of blending (e.g. for the editor).
    bool interpolate = true;

private:
    World world_;
    std::vector<CollisionEvent2D> events_;
    std::vector<BodyId> dead_;
};

}  // namespace mge::physics2d
