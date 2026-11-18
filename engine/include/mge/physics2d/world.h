#pragma once
// physics2d world (M1 #19): rigid bodies, a sequential impulse solver, sleeping and events.
//  - Fixed timestep: advance(frameDt) runs whole steps of `fixedDt` from an accumulator and
//    returns the leftover fraction, so rendering can interpolate between the last two poses
//    (interpolatedTransform). Frame rate never changes the simulation; it stays deterministic.
//  - Each step: new broadphase pairs -> narrowphase (manifolds, events) -> integrate velocities
//    -> islands (waking) -> warm-started sequential impulses (friction, then normal, then a
//    restitution pass) -> integrate positions -> non-linear position correction -> sleep ->
//    broadphase sync. Contact impulses are carried across steps by manifold feature id.
//  - Speculative contacts (made kSpeculativeDistance early) let the solver stop bodies exactly at
//    the surface instead of correcting penetration afterwards.
//  - Islands: bodies linked by touching contacts. An island sleeps when every body in it has
//    been slow for timeToSleep; sleeping bodies cost nothing (no narrowphase, no solve, no
//    broadphase). Anything touching an awake body is woken with its whole island.
//  - Events are collected per advance()/step(): contact begin/end, hits (approach speed above
//    hitEventThreshold) and sensor begin/end. Sensors detect overlap and never push back.
//  - Ids are generational handles: a destroyed body's id never aliases a new body.
// Body and contact storage are arrays with free lists; after warm-up a step allocates nothing.
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "mge/physics2d/broadphase.h"
#include "mge/physics2d/manifold.h"
#include "mge/physics2d/shapes.h"

namespace mge::physics2d {

struct BodyId {
    int32_t index = -1;
    uint32_t generation = 0;
    bool valid() const { return index >= 0; }
    bool operator==(const BodyId& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const BodyId& o) const { return !(*this == o); }
};
struct ShapeId {
    int32_t index = -1;
    uint32_t generation = 0;
    bool valid() const { return index >= 0; }
    bool operator==(const ShapeId& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const ShapeId& o) const { return !(*this == o); }
};

enum class BodyType : uint8_t {
    Static,      // never moves, infinite mass
    Kinematic,   // moved by its velocity only, infinite mass, pushes dynamic bodies
    Dynamic,     // moved by forces and contacts
};

struct BodyDef {
    BodyType type = BodyType::Dynamic;
    vec2 position{};            // body origin (shapes are placed relative to it)
    float angle = 0.0f;
    vec2 linearVelocity{};
    float angularVelocity = 0.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    float gravityScale = 1.0f;
    bool fixedRotation = false;
    bool allowSleep = true;
    bool awake = true;
    uint64_t userData = 0;
};

// Collision filtering: A and B collide when (A.category & B.mask) && (B.category & A.mask),
// unless they share a non-zero group: positive = always collide, negative = never.
struct Filter {
    uint32_t categoryBits = 1;
    uint32_t maskBits = 0xFFFFFFFFu;
    int32_t groupIndex = 0;
};

struct ShapeDef {
    Shape shape;                 // in body-local coordinates
    float density = 1.0f;        // kg/m^2
    float friction = 0.6f;       // mixed as sqrt(a * b)
    float restitution = 0.0f;    // mixed as max(a, b)
    bool isSensor = false;       // reports overlap events, never collides
    Filter filter{};
    uint64_t userData = 0;
};

// Every event also carries both bodies' userData, so it stays meaningful after a shape or body
// was destroyed (end events fire from destroyShape/destroyBody).
struct ContactBeginEvent { ShapeId a, b; uint64_t userA, userB; vec2 normal; vec2 point; };   // normal: a -> b
struct ContactEndEvent { ShapeId a, b; uint64_t userA, userB; };
struct ContactHitEvent { ShapeId a, b; uint64_t userA, userB; vec2 normal; vec2 point; float approachSpeed; };
struct SensorBeginEvent { ShapeId sensor, visitor; uint64_t userSensor, userVisitor; };
struct SensorEndEvent { ShapeId sensor, visitor; uint64_t userSensor, userVisitor; };

struct WorldDef {
    vec2 gravity{0.0f, -10.0f};
    float fixedDt = 1.0f / 60.0f;
    int maxStepsPerAdvance = 4;          // excess time is dropped (no spiral of death)
    int velocityIterations = 8;
    int positionIterations = 3;
    float restitutionThreshold = 1.0f;   // m/s; slower impacts do not bounce
    float hitEventThreshold = 1.0f;      // m/s approach speed for ContactHitEvent
    float linearSleepTolerance = 0.01f;  // m/s
    float angularSleepTolerance = 2.0f * (kPi / 180.0f);   // rad/s
    float timeToSleep = 0.5f;            // s
    bool enableSleep = true;
};

struct WorldStats {
    int32_t bodies = 0, awakeBodies = 0, shapes = 0;
    int32_t contacts = 0, touchingContacts = 0, islands = 0, sleepingIslands = 0;
    int32_t stepsLastAdvance = 0;
    double stepMs = 0.0;          // wall time of the last step()
    size_t memoryBytes = 0;       // capacity of the world's arrays
};

class World {
public:
    explicit World(const WorldDef& def = {});
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- bodies and shapes
    BodyId createBody(const BodyDef& def);
    void destroyBody(BodyId id);   // destroys its shapes; touching contacts report end events
    bool isValid(BodyId id) const;
    ShapeId createShape(BodyId body, const ShapeDef& def);   // updates the body's mass
    void destroyShape(ShapeId id);
    bool isValid(ShapeId id) const;

    // ---- body state
    BodyType bodyType(BodyId id) const;
    Transform2 transform(BodyId id) const;
    vec2 position(BodyId id) const { return transform(id).p; }
    float angle(BodyId id) const { return transform(id).q.angle(); }
    vec2 worldCenter(BodyId id) const;
    void setTransform(BodyId id, vec2 position, float angle);   // teleport; wakes the body
    vec2 linearVelocity(BodyId id) const;
    float angularVelocity(BodyId id) const;
    void setLinearVelocity(BodyId id, vec2 v);
    void setAngularVelocity(BodyId id, float w);
    void applyForce(BodyId id, vec2 force, vec2 worldPoint, bool wake = true);
    void applyForceToCenter(BodyId id, vec2 force, bool wake = true);
    void applyTorque(BodyId id, float torque, bool wake = true);
    void applyLinearImpulse(BodyId id, vec2 impulse, vec2 worldPoint, bool wake = true);
    void applyLinearImpulseToCenter(BodyId id, vec2 impulse, bool wake = true);
    float mass(BodyId id) const;
    float inertia(BodyId id) const;   // about the centre of mass
    bool isAwake(BodyId id) const;
    void setAwake(BodyId id, bool awake);   // waking or sleeping acts on the whole island
    uint64_t userData(BodyId id) const;
    void setUserData(BodyId id, uint64_t data);
    // Pose blended between the previous and the current step: alpha from advance().
    Transform2 interpolatedTransform(BodyId id, float alpha) const;

    // ---- shape state
    BodyId shapeBody(ShapeId id) const;
    const Shape& shape(ShapeId id) const;
    uint64_t shapeUserData(ShapeId id) const;
    bool isSensor(ShapeId id) const;
    void setFilter(ShapeId id, const Filter& f);   // re-pairs the shape next step
    AABB shapeAABB(ShapeId id) const;

    // ---- simulation
    // Runs whole fixed steps for the elapsed frame time; returns the interpolation alpha [0,1).
    float advance(float frameDt);
    // One step of `dt` (normally fixedDt). Clears the events first only when called directly.
    void step(float dt);
    float fixedDt() const { return def_.fixedDt; }
    void setGravity(vec2 g) { def_.gravity = g; }
    vec2 gravity() const { return def_.gravity; }
    WorldDef& settings() { return def_; }

    // ---- events of the last advance()/step()
    const std::vector<ContactBeginEvent>& contactBeginEvents() const { return beginEvents_; }
    const std::vector<ContactEndEvent>& contactEndEvents() const { return endEvents_; }
    const std::vector<ContactHitEvent>& contactHitEvents() const { return hitEvents_; }
    const std::vector<SensorBeginEvent>& sensorBeginEvents() const { return sensorBeginEvents_; }
    const std::vector<SensorEndEvent>& sensorEndEvents() const { return sensorEndEvents_; }

    // ---- queries
    // callback(ShapeId) -> bool (false stops). Shapes whose fat AABB overlaps the box.
    template <class F> void queryAABB(const AABB& box, F&& callback) const;
    // Closest hit along p1 -> p2 among shapes passing the filter (sensors skipped).
    struct RayResult { ShapeId shape; vec2 point{}, normal{}; float fraction = 1.0f; bool hit = false; };
    RayResult rayCastClosest(vec2 p1, vec2 p2, const Filter& filter = {}) const;
    // True when the point is inside a non-sensor shape; the first such shape goes to `out`.
    bool testPoint(vec2 p, ShapeId* out = nullptr) const;

    // callback(BodyId) for every live body.
    template <class F> void eachBody(F&& callback) const;

    const WorldStats& stats() const { return stats_; }
    int32_t bodyCount() const { return bodyCount_; }
    int32_t contactCount() const { return contactCount_; }

private:
    struct Body {
        Transform2 xf, xf0;          // current and previous step (origin pose)
        vec2 localCenter{};
        vec2 c{}, c0{};              // world centre of mass, current and previous step
        vec2 v{};
        float w = 0.0f;
        vec2 force{};
        float torque = 0.0f;
        float mass = 0.0f, invMass = 0.0f, inertia = 0.0f, invI = 0.0f;
        float linearDamping = 0.0f, angularDamping = 0.0f, gravityScale = 1.0f;
        float sleepTime = 0.0f;
        uint64_t userData = 0;
        uint32_t generation = 0;
        int32_t firstShape = -1;
        int32_t islandFlag = 0;       // last step the body was added to an island
        int32_t solverIndex = -1;     // index into the solver arrays this step
        BodyType type = BodyType::Dynamic;
        bool alive = false, awake = true, allowSleep = true, fixedRotation = false;
    };
    struct ShapeRec {
        Shape shape;
        Filter filter;
        float density = 1.0f, friction = 0.6f, restitution = 0.0f;
        uint64_t userData = 0;
        int32_t body = -1;
        int32_t next = -1;            // next shape on the same body
        int32_t proxy = kNullProxy;
        uint32_t generation = 0;
        bool sensor = false, alive = false;
    };
    struct ContactPoint {
        vec2 rA{}, rB{};              // anchors relative to the centres of mass (world-aligned)
        float separation = 0.0f;
        float normalImpulse = 0.0f, tangentImpulse = 0.0f, maxNormalImpulse = 0.0f;
        float normalMass = 0.0f, tangentMass = 0.0f;
        float relativeVelocity = 0.0f;   // normal velocity before the solve (restitution, hits)
        float adjustedSeparation = 0.0f;
        uint32_t id = 0;
    };
    struct Contact {
        int32_t shapeA = -1, shapeB = -1;   // shapeA < shapeB
        Manifold manifold;
        ContactPoint points[2];
        float friction = 0.0f, restitution = 0.0f;
        // Two-point block solver: K = [k11 k12; k12 k22] and its inverse, used when well conditioned.
        float k11 = 0.0f, k12 = 0.0f, k22 = 0.0f, i11 = 0.0f, i12 = 0.0f, i22 = 0.0f;
        bool block = false;
        int32_t next = -1;            // free list
        bool alive = false, touching = false, sensor = false, enabled = true;
    };

    static uint64_t pairKey(int32_t a, int32_t b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }
    Body* body(BodyId id);
    const Body* body(BodyId id) const;
    ShapeRec* shapeRec(ShapeId id);
    const ShapeRec* shapeRec(ShapeId id) const;
    ShapeId shapeIdOf(int32_t index) const { return {index, shapes_[size_t(index)].generation}; }
    uint64_t shapeBodyUser(int32_t index) const { return bodies_[size_t(shapes_[size_t(index)].body)].userData; }
    void updateMass(Body& b);
    void wake(int32_t bodyIndex);
    bool shouldCollide(const ShapeRec& a, const ShapeRec& b) const;
    void addPair(int32_t shapeA, int32_t shapeB);
    void destroyContact(int32_t contactIndex, bool emitEnd);
    void updateContacts();
    void solve(float h);
    void synchronizeShapes(int32_t bodyIndex, vec2 displacement);
    void clearEvents();

    WorldDef def_;
    BroadPhase broadphase_;
    std::vector<Body> bodies_;
    std::vector<int32_t> freeBodies_;
    std::vector<ShapeRec> shapes_;
    std::vector<int32_t> freeShapes_;
    std::vector<Contact> contacts_;
    int32_t freeContact_ = -1;
    std::unordered_map<uint64_t, int32_t> contactMap_;
    int32_t bodyCount_ = 0, shapeCount_ = 0, contactCount_ = 0;
    float accumulator_ = 0.0f;
    int32_t stepIndex_ = 0;
    bool inAdvance_ = false;

    // Per-step scratch (capacity kept between steps).
    std::vector<int32_t> awakeBodies_;      // solver body list
    std::vector<int32_t> solverContacts_;   // touching, enabled, non-sensor contacts to solve
    std::vector<int32_t> adjStart_, adjList_, stack_, island_;

    std::vector<ContactBeginEvent> beginEvents_;
    std::vector<ContactEndEvent> endEvents_;
    std::vector<ContactHitEvent> hitEvents_;
    std::vector<SensorBeginEvent> sensorBeginEvents_;
    std::vector<SensorEndEvent> sensorEndEvents_;
    WorldStats stats_;
};

template <class F> void World::queryAABB(const AABB& box, F&& callback) const {
    broadphase_.tree().query(box, [&](int32_t proxy) {
        const int32_t s = int32_t(broadphase_.tree().userData(proxy));
        return callback(shapeIdOf(s));
    });
}

template <class F> void World::eachBody(F&& callback) const {
    for (size_t i = 0; i < bodies_.size(); ++i)
        if (bodies_[i].alive) callback(BodyId{int32_t(i), bodies_[i].generation});
}

}  // namespace mge::physics2d
