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
#include "mge/serialize.h"
#include "mge/transform2d.h"

namespace mge::physics2d {

struct RigidBody2D {
    BodyId body;
    uint32_t wasAwake = 1;   // internal: lets the final pose be written once when it falls asleep
};

// Authoring data for a body with one shape (M2 #24): what the editor edits and scenes save.
// Play mode (or a game at load) turns it into a RigidBody2D with createFromColliders().
//  - Shapes scale with the entity's world scale: a 1x1 box on an entity scaled 2x3 is 2x3.
//  - Root entities simulate (their Transform2D follows the body). Colliders on child entities
//    become static bodies at their world pose: bodies drive root transforms only.
struct Collider2D {
    uint32_t bodyType = 2;        // BodyType: 0 Static, 1 Kinematic, 2 Dynamic
    uint32_t shape = 0;           // 0 Box, 1 Circle
    vec2 size{1, 1};              // box size, or circle diameter in size.x
    vec2 offset{};                // shape centre in entity-local units
    float density = 1.0f;
    float friction = 0.6f;
    float restitution = 0.0f;
    float gravityScale = 1.0f;
    uint32_t fixedRotation = 0;
    uint32_t sensor = 0;
};

// Registers Collider2D (enum names for bodyType and shape, bool hints).
void registerPhysics2D(serial::Registry& reg);

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

    // Creates a body for every entity with a Collider2D (see Collider2D). Run TransformSystem2D
    // first: shapes and child poses come from WorldTransform2D. Returns the number of bodies.
    uint32_t createFromColliders(ecs::World& w);

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
