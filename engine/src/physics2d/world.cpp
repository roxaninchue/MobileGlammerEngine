#include "mge/physics2d/world.h"
#include <chrono>
#include <cmath>
#include <utility>

namespace mge::physics2d {

namespace {

constexpr float kBaumgarte = 0.2f;            // fraction of penetration removed per position iteration
constexpr float kMaxCorrection = 0.2f;        // m per position iteration
constexpr float kMaxTranslation = 4.0f;       // m per step
constexpr float kMaxRotation = 0.5f * kPi;    // rad per step

inline vec2 crossSV(float s, vec2 v) { return {-s * v.y, s * v.x}; }   // s x v
inline Rot integrateRot(Rot q, float dAngle) {
    // q + dAngle * dq/dangle, renormalised: exact enough for per-step angles, no trig.
    const float c = q.c - dAngle * q.s, s = q.s + dAngle * q.c;
    const float inv = 1.0f / std::sqrt(c * c + s * s);
    return {c * inv, s * inv};
}

}  // namespace

World::World(const WorldDef& def) : def_(def) {}

// ------------------------------------------------------------------ access

World::Body* World::body(BodyId id) {
    if (id.index < 0 || size_t(id.index) >= bodies_.size()) return nullptr;
    Body& b = bodies_[size_t(id.index)];
    return b.alive && b.generation == id.generation ? &b : nullptr;
}
const World::Body* World::body(BodyId id) const { return const_cast<World*>(this)->body(id); }

World::ShapeRec* World::shapeRec(ShapeId id) {
    if (id.index < 0 || size_t(id.index) >= shapes_.size()) return nullptr;
    ShapeRec& s = shapes_[size_t(id.index)];
    return s.alive && s.generation == id.generation ? &s : nullptr;
}
const World::ShapeRec* World::shapeRec(ShapeId id) const { return const_cast<World*>(this)->shapeRec(id); }

bool World::isValid(BodyId id) const { return body(id) != nullptr; }
bool World::isValid(ShapeId id) const { return shapeRec(id) != nullptr; }

// ------------------------------------------------------------------ bodies

BodyId World::createBody(const BodyDef& def) {
    int32_t index;
    if (!freeBodies_.empty()) {
        index = freeBodies_.back();
        freeBodies_.pop_back();
    } else {
        index = int32_t(bodies_.size());
        bodies_.emplace_back();
    }
    Body& b = bodies_[size_t(index)];
    const uint32_t gen = b.generation + 1;
    b = Body{};
    b.generation = gen;
    b.alive = true;
    b.type = def.type;
    b.xf = b.xf0 = Transform2::make(def.position, def.angle);
    b.c = b.c0 = def.position;
    b.v = def.type == BodyType::Static ? vec2{} : def.linearVelocity;
    b.w = def.type == BodyType::Static ? 0.0f : def.angularVelocity;
    b.linearDamping = def.linearDamping;
    b.angularDamping = def.angularDamping;
    b.gravityScale = def.gravityScale;
    b.fixedRotation = def.fixedRotation;
    b.allowSleep = def.allowSleep;
    b.awake = def.type != BodyType::Static && def.awake;
    b.userData = def.userData;
    updateMass(b);
    ++bodyCount_;
    return {index, gen};
}

void World::destroyBody(BodyId id) {
    Body* b = body(id);
    if (!b) return;
    while (b->firstShape >= 0) destroyShape(shapeIdOf(b->firstShape));
    b->alive = false;
    freeBodies_.push_back(id.index);
    --bodyCount_;
}

ShapeId World::createShape(BodyId bodyId, const ShapeDef& def) {
    Body* b = body(bodyId);
    if (!b) return {};
    int32_t index;
    if (!freeShapes_.empty()) {
        index = freeShapes_.back();
        freeShapes_.pop_back();
    } else {
        index = int32_t(shapes_.size());
        shapes_.emplace_back();
    }
    ShapeRec& s = shapes_[size_t(index)];
    const uint32_t gen = s.generation + 1;
    s = ShapeRec{};
    s.generation = gen;
    s.alive = true;
    s.shape = def.shape;
    s.filter = def.filter;
    s.density = def.density;
    s.friction = def.friction;
    s.restitution = def.restitution;
    s.sensor = def.isSensor;
    s.userData = def.userData;
    s.body = bodyId.index;
    s.next = b->firstShape;
    b->firstShape = index;
    s.proxy = broadphase_.createProxy(computeAABB(s.shape, b->xf), uint64_t(index));
    updateMass(*b);
    ++shapeCount_;
    return {index, gen};
}

void World::destroyShape(ShapeId id) {
    ShapeRec* s = shapeRec(id);
    if (!s) return;
    // Contacts first (they report end events while the shape is still valid).
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& c = contacts_[i];
        if (c.alive && (c.shapeA == id.index || c.shapeB == id.index)) destroyContact(int32_t(i), true);
    }
    broadphase_.destroyProxy(s->proxy);
    Body& b = bodies_[size_t(s->body)];
    for (int32_t* link = &b.firstShape; *link >= 0; link = &shapes_[size_t(*link)].next) {
        if (*link == id.index) { *link = s->next; break; }
    }
    s->alive = false;
    s->proxy = kNullProxy;
    freeShapes_.push_back(id.index);
    --shapeCount_;
    updateMass(b);
}

void World::updateMass(Body& b) {
    b.mass = b.invMass = b.inertia = b.invI = 0.0f;
    b.localCenter = {};
    if (b.type != BodyType::Dynamic) {
        b.c = b.xf.p;
        return;
    }
    float mass = 0.0f;
    vec2 center{};
    for (int32_t s = b.firstShape; s >= 0; s = shapes_[size_t(s)].next) {
        const ShapeRec& sh = shapes_[size_t(s)];
        if (sh.sensor || sh.density <= 0.0f) continue;
        const MassData md = computeMass(sh.shape, sh.density);
        mass += md.mass;
        center += md.center * md.mass;
    }
    if (mass > 0.0f) {
        center = center / mass;
        float inertia = 0.0f;
        for (int32_t s = b.firstShape; s >= 0; s = shapes_[size_t(s)].next) {
            const ShapeRec& sh = shapes_[size_t(s)];
            if (sh.sensor || sh.density <= 0.0f) continue;
            const MassData md = computeMass(sh.shape, sh.density);
            inertia += md.inertia + md.mass * lengthSq(md.center - center);   // parallel axis
        }
        b.mass = mass;
        b.inertia = inertia;
    } else {
        b.mass = 1.0f;   // shapeless or massless dynamic bodies still fall
    }
    b.invMass = 1.0f / b.mass;
    b.invI = (!b.fixedRotation && b.inertia > 0.0f) ? 1.0f / b.inertia : 0.0f;
    if (b.fixedRotation) b.inertia = 0.0f;
    b.localCenter = center;
    const vec2 oldC = b.c;
    b.c = b.xf.apply(center);
    b.c0 = b.c;
    b.v += crossSV(b.w, b.c - oldC);   // keep the velocity of the new centre consistent
}

BodyType World::bodyType(BodyId id) const { const Body* b = body(id); return b ? b->type : BodyType::Static; }
Transform2 World::transform(BodyId id) const { const Body* b = body(id); return b ? b->xf : Transform2{}; }
vec2 World::worldCenter(BodyId id) const { const Body* b = body(id); return b ? b->c : vec2{}; }
vec2 World::linearVelocity(BodyId id) const { const Body* b = body(id); return b ? b->v : vec2{}; }
float World::angularVelocity(BodyId id) const { const Body* b = body(id); return b ? b->w : 0.0f; }
float World::mass(BodyId id) const { const Body* b = body(id); return b ? b->mass : 0.0f; }
float World::inertia(BodyId id) const { const Body* b = body(id); return b ? b->inertia : 0.0f; }
bool World::isAwake(BodyId id) const { const Body* b = body(id); return b && b->awake; }
uint64_t World::userData(BodyId id) const { const Body* b = body(id); return b ? b->userData : 0; }
void World::setUserData(BodyId id, uint64_t d) { if (Body* b = body(id)) b->userData = d; }

void World::setTransform(BodyId id, vec2 position, float angle) {
    Body* b = body(id);
    if (!b) return;
    b->xf = b->xf0 = Transform2::make(position, angle);
    b->c = b->c0 = b->xf.apply(b->localCenter);
    for (int32_t s = b->firstShape; s >= 0; s = shapes_[size_t(s)].next)
        broadphase_.moveProxy(shapes_[size_t(s)].proxy, computeAABB(shapes_[size_t(s)].shape, b->xf));
    if (b->type != BodyType::Static) wake(id.index);
}

void World::setLinearVelocity(BodyId id, vec2 v) {
    Body* b = body(id);
    if (!b || b->type == BodyType::Static) return;
    if (lengthSq(v) > 0.0f) wake(id.index);
    b->v = v;
}
void World::setAngularVelocity(BodyId id, float w) {
    Body* b = body(id);
    if (!b || b->type == BodyType::Static || b->fixedRotation) return;
    if (w != 0.0f) wake(id.index);
    b->w = w;
}

void World::applyForce(BodyId id, vec2 f, vec2 point, bool doWake) {
    Body* b = body(id);
    if (!b || b->type != BodyType::Dynamic) return;
    if (doWake) wake(id.index);
    if (!b->awake) return;
    b->force += f;
    b->torque += cross(point - b->c, f);
}
void World::applyForceToCenter(BodyId id, vec2 f, bool doWake) {
    Body* b = body(id);
    if (!b || b->type != BodyType::Dynamic) return;
    if (doWake) wake(id.index);
    if (b->awake) b->force += f;
}
void World::applyTorque(BodyId id, float t, bool doWake) {
    Body* b = body(id);
    if (!b || b->type != BodyType::Dynamic) return;
    if (doWake) wake(id.index);
    if (b->awake) b->torque += t;
}
void World::applyLinearImpulse(BodyId id, vec2 p, vec2 point, bool doWake) {
    Body* b = body(id);
    if (!b || b->type != BodyType::Dynamic) return;
    if (doWake) wake(id.index);
    if (!b->awake) return;
    b->v += p * b->invMass;
    b->w += b->invI * cross(point - b->c, p);
}
void World::applyLinearImpulseToCenter(BodyId id, vec2 p, bool doWake) {
    Body* b = body(id);
    if (!b || b->type != BodyType::Dynamic) return;
    if (doWake) wake(id.index);
    if (b->awake) b->v += p * b->invMass;
}

void World::wake(int32_t i) {
    Body& b = bodies_[size_t(i)];
    if (b.type == BodyType::Static) return;
    b.awake = true;
    b.sleepTime = 0.0f;
}

void World::setAwake(BodyId id, bool awake) {
    Body* b = body(id);
    if (!b || b->type == BodyType::Static) return;
    if (awake) {
        wake(id.index);   // the next step's island search wakes everything it touches
        return;
    }
    // Put the body and everything touching it (transitively) to sleep.
    b->awake = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Contact& c : contacts_) {
            if (!c.alive || !c.touching || c.sensor) continue;
            Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
            Body& o = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
            if (a.type == BodyType::Static || o.type == BodyType::Static) continue;
            if (a.awake != o.awake) { a.awake = o.awake = false; changed = true; }
        }
    }
    for (Body& x : bodies_) {
        if (x.alive && !x.awake && x.type != BodyType::Static) {
            x.v = {};
            x.w = 0.0f;
            x.force = {};
            x.torque = 0.0f;
            x.xf0 = x.xf;
            x.c0 = x.c;
        }
    }
}

Transform2 World::interpolatedTransform(BodyId id, float alpha) const {
    const Body* b = body(id);
    if (!b) return {};
    if (!b->awake || alpha >= 1.0f) return b->xf;
    Transform2 t;
    t.p = lerp(b->xf0.p, b->xf.p, alpha);
    const float c = lerp(b->xf0.q.c, b->xf.q.c, alpha), s = lerp(b->xf0.q.s, b->xf.q.s, alpha);
    const float len = std::sqrt(c * c + s * s);
    t.q = len > kEpsilon ? Rot{c / len, s / len} : b->xf.q;
    return t;
}

// ------------------------------------------------------------------ shapes

BodyId World::shapeBody(ShapeId id) const {
    const ShapeRec* s = shapeRec(id);
    return s ? BodyId{s->body, bodies_[size_t(s->body)].generation} : BodyId{};
}
const Shape& World::shape(ShapeId id) const {
    static const Shape kNone;
    const ShapeRec* s = shapeRec(id);
    return s ? s->shape : kNone;
}
uint64_t World::shapeUserData(ShapeId id) const { const ShapeRec* s = shapeRec(id); return s ? s->userData : 0; }
bool World::isSensor(ShapeId id) const { const ShapeRec* s = shapeRec(id); return s && s->sensor; }
AABB World::shapeAABB(ShapeId id) const {
    const ShapeRec* s = shapeRec(id);
    return s ? computeAABB(s->shape, bodies_[size_t(s->body)].xf) : AABB{};
}

void World::setFilter(ShapeId id, const Filter& f) {
    ShapeRec* s = shapeRec(id);
    if (!s) return;
    s->filter = f;
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& c = contacts_[i];
        if (c.alive && (c.shapeA == id.index || c.shapeB == id.index) &&
            !shouldCollide(shapes_[size_t(c.shapeA)], shapes_[size_t(c.shapeB)]))
            destroyContact(int32_t(i), true);
    }
    broadphase_.touchProxy(s->proxy);
}

bool World::shouldCollide(const ShapeRec& a, const ShapeRec& b) const {
    if (a.body == b.body) return false;
    if (a.sensor && b.sensor) return false;
    const BodyType ta = bodies_[size_t(a.body)].type, tb = bodies_[size_t(b.body)].type;
    if (a.sensor || b.sensor) {
        if (ta == BodyType::Static && tb == BodyType::Static) return false;
    } else if (ta != BodyType::Dynamic && tb != BodyType::Dynamic) {
        return false;
    }
    if (a.filter.groupIndex != 0 && a.filter.groupIndex == b.filter.groupIndex) return a.filter.groupIndex > 0;
    return (a.filter.categoryBits & b.filter.maskBits) && (b.filter.categoryBits & a.filter.maskBits);
}

// ------------------------------------------------------------------ contacts

void World::addPair(int32_t sa, int32_t sb) {
    if (sa > sb) std::swap(sa, sb);
    const ShapeRec& a = shapes_[size_t(sa)];
    const ShapeRec& b = shapes_[size_t(sb)];
    if (!a.alive || !b.alive || !shouldCollide(a, b)) return;
    const uint64_t key = pairKey(sa, sb);
    if (contactMap_.find(key) != contactMap_.end()) return;
    int32_t index;
    if (freeContact_ >= 0) {
        index = freeContact_;
        freeContact_ = contacts_[size_t(index)].next;
    } else {
        index = int32_t(contacts_.size());
        contacts_.emplace_back();
    }
    Contact& c = contacts_[size_t(index)];
    c = Contact{};
    c.alive = true;
    c.shapeA = sa;
    c.shapeB = sb;
    c.sensor = a.sensor || b.sensor;
    c.friction = std::sqrt(a.friction * b.friction);
    c.restitution = max(a.restitution, b.restitution);
    contactMap_.emplace(key, index);
    ++contactCount_;
}

void World::destroyContact(int32_t ci, bool emitEnd) {
    Contact& c = contacts_[size_t(ci)];
    if (!c.alive) return;
    if (c.touching && emitEnd) {
        if (c.sensor) {
            const bool aSensor = shapes_[size_t(c.shapeA)].sensor;
            const int32_t sensor = aSensor ? c.shapeA : c.shapeB, visitor = aSensor ? c.shapeB : c.shapeA;
            sensorEndEvents_.push_back({shapeIdOf(sensor), shapeIdOf(visitor), shapeBodyUser(sensor), shapeBodyUser(visitor)});
        } else {
            endEvents_.push_back({shapeIdOf(c.shapeA), shapeIdOf(c.shapeB), shapeBodyUser(c.shapeA), shapeBodyUser(c.shapeB)});
            // Whatever rested on this contact has to react to it being gone.
            wake(shapes_[size_t(c.shapeA)].body);
            wake(shapes_[size_t(c.shapeB)].body);
        }
    }
    contactMap_.erase(pairKey(c.shapeA, c.shapeB));
    c.alive = false;
    c.touching = false;
    c.next = freeContact_;
    freeContact_ = ci;
    --contactCount_;
}

void World::updateContacts() {
    for (size_t i = 0; i < contacts_.size(); ++i) {
        Contact& c = contacts_[i];
        if (!c.alive) continue;
        const ShapeRec& sa = shapes_[size_t(c.shapeA)];
        const ShapeRec& sb = shapes_[size_t(c.shapeB)];
        const Body& ba = bodies_[size_t(sa.body)];
        const Body& bb = bodies_[size_t(sb.body)];
        if (!ba.awake && !bb.awake) continue;   // both asleep or static: nothing can change
        if (!broadphase_.testOverlap(sa.proxy, sb.proxy)) {
            destroyContact(int32_t(i), true);
            continue;
        }
        const Manifold m = collide(sa.shape, ba.xf, sb.shape, bb.xf);
        const bool was = c.touching;
        if (c.sensor) {
            bool overlap = false;
            for (int k = 0; k < m.count; ++k) overlap |= m.points[k].separation < 0.0f;
            c.touching = overlap;
            c.manifold = m;
            if (overlap != was) {
                const int32_t sensor = sa.sensor ? c.shapeA : c.shapeB, visitor = sa.sensor ? c.shapeB : c.shapeA;
                if (overlap) sensorBeginEvents_.push_back({shapeIdOf(sensor), shapeIdOf(visitor), shapeBodyUser(sensor), shapeBodyUser(visitor)});
                else sensorEndEvents_.push_back({shapeIdOf(sensor), shapeIdOf(visitor), shapeBodyUser(sensor), shapeBodyUser(visitor)});
            }
            continue;
        }
        // Carry impulses over by feature id (warm starting).
        ContactPoint old[2] = {c.points[0], c.points[1]};
        const int oldCount = c.manifold.count;
        c.manifold = m;
        for (int k = 0; k < m.count; ++k) {
            ContactPoint& p = c.points[k];
            p = ContactPoint{};
            p.id = m.points[k].id;
            p.separation = m.points[k].separation;
            for (int j = 0; j < oldCount; ++j) {
                if (old[j].id == p.id) {
                    p.normalImpulse = old[j].normalImpulse;
                    p.tangentImpulse = old[j].tangentImpulse;
                    break;
                }
            }
        }
        // Touching includes speculative points: the solver needs them before the surfaces meet.
        c.touching = m.count > 0;
        if (c.touching && !was) {
            beginEvents_.push_back({shapeIdOf(c.shapeA), shapeIdOf(c.shapeB), shapeBodyUser(c.shapeA), shapeBodyUser(c.shapeB), m.normal, m.points[0].point});
        } else if (!c.touching && was) {
            endEvents_.push_back({shapeIdOf(c.shapeA), shapeIdOf(c.shapeB), shapeBodyUser(c.shapeA), shapeBodyUser(c.shapeB)});
        }
    }
}

// ------------------------------------------------------------------ step

void World::clearEvents() {
    beginEvents_.clear();
    endEvents_.clear();
    hitEvents_.clear();
    sensorBeginEvents_.clear();
    sensorEndEvents_.clear();
}

float World::advance(float frameDt) {
    clearEvents();
    inAdvance_ = true;
    accumulator_ += clamp(frameDt, 0.0f, 0.25f);
    int steps = 0;
    while (accumulator_ >= def_.fixedDt && steps < def_.maxStepsPerAdvance) {
        step(def_.fixedDt);
        accumulator_ -= def_.fixedDt;
        ++steps;
    }
    if (accumulator_ >= def_.fixedDt) accumulator_ = std::fmod(accumulator_, def_.fixedDt);   // drop backlog
    inAdvance_ = false;
    stats_.stepsLastAdvance = steps;
    return accumulator_ / def_.fixedDt;
}

void World::step(float dt) {
    if (!inAdvance_) clearEvents();
    if (dt <= 0.0f) return;
    const auto t0 = std::chrono::steady_clock::now();
    ++stepIndex_;
    broadphase_.updatePairs([&](uint64_t a, uint64_t b) { addPair(int32_t(a), int32_t(b)); });
    updateContacts();
    solve(dt);

    stats_.bodies = bodyCount_;
    stats_.shapes = shapeCount_;
    stats_.contacts = contactCount_;
    stats_.stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    stats_.memoryBytes = bodies_.capacity() * sizeof(Body) + shapes_.capacity() * sizeof(ShapeRec) +
                         contacts_.capacity() * sizeof(Contact) + contactMap_.size() * 32 +
                         (awakeBodies_.capacity() + solverContacts_.capacity() + adjStart_.capacity() +
                          adjList_.capacity() + stack_.capacity() + island_.capacity()) * sizeof(int32_t);
}

void World::synchronizeShapes(int32_t bi, vec2 displacement) {
    const Body& b = bodies_[size_t(bi)];
    for (int32_t s = b.firstShape; s >= 0; s = shapes_[size_t(s)].next)
        broadphase_.moveProxy(shapes_[size_t(s)].proxy, computeAABB(shapes_[size_t(s)].shape, b.xf), displacement);
}

void World::solve(float h) {
    const float invH = 1.0f / h;

    // ---- adjacency over touching contacts (CSR), used by the island search
    const size_t nb = bodies_.size();
    adjStart_.assign(nb + 1, 0);
    for (const Contact& c : contacts_) {
        if (!c.alive || !c.touching || c.sensor) continue;
        ++adjStart_[size_t(shapes_[size_t(c.shapeA)].body) + 1];
        ++adjStart_[size_t(shapes_[size_t(c.shapeB)].body) + 1];
    }
    for (size_t i = 0; i < nb; ++i) adjStart_[i + 1] += adjStart_[i];
    adjList_.resize(size_t(adjStart_[nb]));
    stack_.assign(adjStart_.begin(), adjStart_.end() - 1);   // fill cursors
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& c = contacts_[i];
        if (!c.alive || !c.touching || c.sensor) continue;
        adjList_[size_t(stack_[size_t(shapes_[size_t(c.shapeA)].body)]++)] = int32_t(i);
        adjList_[size_t(stack_[size_t(shapes_[size_t(c.shapeB)].body)]++)] = int32_t(i);
    }

    // ---- islands: seeded from awake bodies, waking everything they touch
    awakeBodies_.clear();
    island_.clear();   // island boundaries into awakeBodies_ (start indices)
    stats_.islands = 0;
    for (size_t seed = 0; seed < nb; ++seed) {
        Body& s = bodies_[seed];
        if (!s.alive || !s.awake || s.type == BodyType::Static || s.islandFlag == stepIndex_) continue;
        island_.push_back(int32_t(awakeBodies_.size()));
        ++stats_.islands;
        stack_.clear();
        stack_.push_back(int32_t(seed));
        s.islandFlag = stepIndex_;
        while (!stack_.empty()) {
            const int32_t bi = stack_.back();
            stack_.pop_back();
            Body& b = bodies_[size_t(bi)];
            if (!b.awake) wake(bi);
            b.solverIndex = int32_t(awakeBodies_.size());
            awakeBodies_.push_back(bi);
            for (int32_t k = adjStart_[size_t(bi)]; k < adjStart_[size_t(bi) + 1]; ++k) {
                const Contact& c = contacts_[size_t(adjList_[size_t(k)])];
                const int32_t other = shapes_[size_t(c.shapeA)].body == bi ? shapes_[size_t(c.shapeB)].body
                                                                           : shapes_[size_t(c.shapeA)].body;
                Body& o = bodies_[size_t(other)];
                if (o.type == BodyType::Static || o.islandFlag == stepIndex_) continue;   // statics don't link islands
                o.islandFlag = stepIndex_;
                stack_.push_back(other);
            }
        }
    }
    island_.push_back(int32_t(awakeBodies_.size()));

    // ---- integrate velocities, remember the start-of-step pose
    for (int32_t bi : awakeBodies_) {
        Body& b = bodies_[size_t(bi)];
        b.xf0 = b.xf;
        b.c0 = b.c;
        if (b.type != BodyType::Dynamic) continue;
        b.v += (def_.gravity * b.gravityScale + b.force * b.invMass) * h;
        b.w += b.invI * b.torque * h;
        b.v *= 1.0f / (1.0f + h * b.linearDamping);
        b.w *= 1.0f / (1.0f + h * b.angularDamping);
        b.force = {};
        b.torque = 0.0f;
    }

    // ---- contacts to solve: touching, non-sensor, at least one awake body (DFS woke the other)
    solverContacts_.clear();
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& c = contacts_[i];
        if (!c.alive || !c.touching || c.sensor || !c.enabled) continue;
        const Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
        const Body& b = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
        if (!a.awake && !b.awake) continue;
        solverContacts_.push_back(int32_t(i));
    }

    // ---- prepare + warm start
    for (int32_t ci : solverContacts_) {
        Contact& c = contacts_[size_t(ci)];
        Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
        Body& b = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
        const vec2 n = c.manifold.normal, t{n.y, -n.x};
        for (int k = 0; k < c.manifold.count; ++k) {
            ContactPoint& p = c.points[k];
            const vec2 wp = c.manifold.points[k].point;
            p.rA = wp - a.c;
            p.rB = wp - b.c;
            const float rnA = cross(p.rA, n), rnB = cross(p.rB, n);
            const float kn = a.invMass + b.invMass + a.invI * rnA * rnA + b.invI * rnB * rnB;
            p.normalMass = kn > 0.0f ? 1.0f / kn : 0.0f;
            const float rtA = cross(p.rA, t), rtB = cross(p.rB, t);
            const float kt = a.invMass + b.invMass + a.invI * rtA * rtA + b.invI * rtB * rtB;
            p.tangentMass = kt > 0.0f ? 1.0f / kt : 0.0f;
            p.adjustedSeparation = p.separation - dot(p.rB - p.rA, n);
            const vec2 dv = b.v + crossSV(b.w, p.rB) - a.v - crossSV(a.w, p.rA);
            p.relativeVelocity = dot(n, dv);
            p.maxNormalImpulse = 0.0f;
            const vec2 P = n * p.normalImpulse + t * p.tangentImpulse;
            a.v -= P * a.invMass;
            a.w -= a.invI * cross(p.rA, P);
            b.v += P * b.invMass;
            b.w += b.invI * cross(p.rB, P);
        }
        c.block = false;
        if (c.manifold.count == 2) {
            const ContactPoint& p1 = c.points[0];
            const ContactPoint& p2 = c.points[1];
            const float r1A = cross(p1.rA, n), r1B = cross(p1.rB, n), r2A = cross(p2.rA, n), r2B = cross(p2.rB, n);
            c.k11 = a.invMass + b.invMass + a.invI * r1A * r1A + b.invI * r1B * r1B;
            c.k22 = a.invMass + b.invMass + a.invI * r2A * r2A + b.invI * r2B * r2B;
            c.k12 = a.invMass + b.invMass + a.invI * r1A * r2A + b.invI * r1B * r2B;
            const float det = c.k11 * c.k22 - c.k12 * c.k12;
            // Only when well conditioned (points not nearly coincident in effect).
            if (c.k11 * c.k11 < 1000.0f * det && det > 0.0f) {
                const float id = 1.0f / det;
                c.i11 = c.k22 * id;
                c.i12 = -c.k12 * id;
                c.i22 = c.k11 * id;
                c.block = true;
            }
        }
    }

    // ---- velocity iterations: friction, then normal (speculative points allow approach up to s/h)
    for (int it = 0; it < def_.velocityIterations; ++it) {
        for (int32_t ci : solverContacts_) {
            Contact& c = contacts_[size_t(ci)];
            Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
            Body& b = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
            const vec2 n = c.manifold.normal, t{n.y, -n.x};
            for (int k = 0; k < c.manifold.count; ++k) {
                ContactPoint& p = c.points[k];
                const vec2 dv = b.v + crossSV(b.w, p.rB) - a.v - crossSV(a.w, p.rA);
                const float maxF = c.friction * p.normalImpulse;
                const float newT = clamp(p.tangentImpulse - p.tangentMass * dot(dv, t), -maxF, maxF);
                const float lt = newT - p.tangentImpulse;
                p.tangentImpulse = newT;
                const vec2 P = t * lt;
                a.v -= P * a.invMass;
                a.w -= a.invI * cross(p.rA, P);
                b.v += P * b.invMass;
                b.w += b.invI * cross(p.rB, P);
            }
            if (c.block) {
                // Solve both normal impulses together (mixed LCP by enumeration): stacks stay
                // square instead of rocking from point to point.
                ContactPoint& p1 = c.points[0];
                ContactPoint& p2 = c.points[1];
                const vec2 dv1 = b.v + crossSV(b.w, p1.rB) - a.v - crossSV(a.w, p1.rA);
                const vec2 dv2 = b.v + crossSV(b.w, p2.rB) - a.v - crossSV(a.w, p2.rA);
                const float ax = p1.normalImpulse, ay = p2.normalImpulse;
                const float b1 = dot(dv1, n) + (p1.separation > 0.0f ? p1.separation * invH : 0.0f) - (c.k11 * ax + c.k12 * ay);
                const float b2 = dot(dv2, n) + (p2.separation > 0.0f ? p2.separation * invH : 0.0f) - (c.k12 * ax + c.k22 * ay);
                float x1, x2;
                for (;;) {
                    x1 = -(c.i11 * b1 + c.i12 * b2);   // both active
                    x2 = -(c.i12 * b1 + c.i22 * b2);
                    if (x1 >= 0.0f && x2 >= 0.0f) break;
                    x1 = -b1 / c.k11; x2 = 0.0f;       // only point 1
                    if (x1 >= 0.0f && c.k12 * x1 + b2 >= 0.0f) break;
                    x1 = 0.0f; x2 = -b2 / c.k22;       // only point 2
                    if (x2 >= 0.0f && c.k12 * x2 + b1 >= 0.0f) break;
                    x1 = x2 = 0.0f;                    // neither
                    if (b1 >= 0.0f && b2 >= 0.0f) break;
                    x1 = ax; x2 = ay;                  // no solution (numerical): keep last
                    break;
                }
                const vec2 P1 = n * (x1 - ax), P2 = n * (x2 - ay);
                p1.normalImpulse = x1;
                p2.normalImpulse = x2;
                p1.maxNormalImpulse = max(p1.maxNormalImpulse, x1);
                p2.maxNormalImpulse = max(p2.maxNormalImpulse, x2);
                a.v -= (P1 + P2) * a.invMass;
                a.w -= a.invI * (cross(p1.rA, P1) + cross(p2.rA, P2));
                b.v += (P1 + P2) * b.invMass;
                b.w += b.invI * (cross(p1.rB, P1) + cross(p2.rB, P2));
                continue;
            }
            for (int k = 0; k < c.manifold.count; ++k) {
                ContactPoint& p = c.points[k];
                const vec2 dv = b.v + crossSV(b.w, p.rB) - a.v - crossSV(a.w, p.rA);
                const float vn = dot(dv, n);
                const float bias = p.separation > 0.0f ? p.separation * invH : 0.0f;
                const float newN = max(p.normalImpulse - p.normalMass * (vn + bias), 0.0f);
                const float ln = newN - p.normalImpulse;
                p.normalImpulse = newN;
                p.maxNormalImpulse = max(p.maxNormalImpulse, newN);
                const vec2 P = n * ln;
                a.v -= P * a.invMass;
                a.w -= a.invI * cross(p.rA, P);
                b.v += P * b.invMass;
                b.w += b.invI * cross(p.rB, P);
            }
        }
    }

    // ---- restitution: points that took load and hit hard enough bounce back
    for (int32_t ci : solverContacts_) {
        Contact& c = contacts_[size_t(ci)];
        Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
        Body& b = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
        const vec2 n = c.manifold.normal;
        float approach = 0.0f;
        int hitPoint = -1;
        for (int k = 0; k < c.manifold.count; ++k) {
            ContactPoint& p = c.points[k];
            if (p.maxNormalImpulse > 0.0f && -p.relativeVelocity > approach) {
                approach = -p.relativeVelocity;
                hitPoint = k;
            }
            if (c.restitution == 0.0f || p.relativeVelocity > -def_.restitutionThreshold || p.maxNormalImpulse == 0.0f)
                continue;
            const vec2 dv = b.v + crossSV(b.w, p.rB) - a.v - crossSV(a.w, p.rA);
            const float vn = dot(dv, n);
            const float newN = max(p.normalImpulse - p.normalMass * (vn + c.restitution * p.relativeVelocity), 0.0f);
            const float ln = newN - p.normalImpulse;
            p.normalImpulse = newN;
            const vec2 P = n * ln;
            a.v -= P * a.invMass;
            a.w -= a.invI * cross(p.rA, P);
            b.v += P * b.invMass;
            b.w += b.invI * cross(p.rB, P);
        }
        if (hitPoint >= 0 && approach > def_.hitEventThreshold)
            hitEvents_.push_back({shapeIdOf(c.shapeA), shapeIdOf(c.shapeB), shapeBodyUser(c.shapeA), shapeBodyUser(c.shapeB), n, c.manifold.points[hitPoint].point, approach});
    }

    // ---- integrate positions
    for (int32_t bi : awakeBodies_) {
        Body& b = bodies_[size_t(bi)];
        vec2 dp = b.v * h;
        if (lengthSq(dp) > kMaxTranslation * kMaxTranslation) {
            b.v *= kMaxTranslation / length(dp);
            dp = b.v * h;
        }
        float da = b.w * h;
        if (std::fabs(da) > kMaxRotation) {
            b.w *= kMaxRotation / std::fabs(da);
            da = b.w * h;
        }
        b.c += dp;
        b.xf.q = integrateRot(b.xf.q, da);
        b.xf.p = b.c - b.xf.q.rotate(b.localCenter);
    }

    // ---- position correction (non-linear Gauss-Seidel on the remaining penetration)
    for (int it = 0; it < def_.positionIterations; ++it) {
        float minSep = 0.0f;
        for (int32_t ci : solverContacts_) {
            Contact& c = contacts_[size_t(ci)];
            Body& a = bodies_[size_t(shapes_[size_t(c.shapeA)].body)];
            Body& b = bodies_[size_t(shapes_[size_t(c.shapeB)].body)];
            const vec2 n = c.manifold.normal;
            for (int k = 0; k < c.manifold.count; ++k) {
                const ContactPoint& p = c.points[k];
                const vec2 prA = invMul(a.xf0.q, a.xf.q).rotate(p.rA);
                const vec2 prB = invMul(b.xf0.q, b.xf.q).rotate(p.rB);
                const float s = dot(n, (b.c - b.c0) - (a.c - a.c0) + prB - prA) + p.adjustedSeparation;
                minSep = min(minSep, s);
                const float C = clamp(kBaumgarte * (s + kLinearSlop), -kMaxCorrection, 0.0f);
                const float rnA = cross(prA, n), rnB = cross(prB, n);
                const float kn = a.invMass + b.invMass + a.invI * rnA * rnA + b.invI * rnB * rnB;
                if (C == 0.0f || kn <= 0.0f) continue;
                const vec2 P = n * (-C / kn);
                a.c -= P * a.invMass;
                a.xf.q = integrateRot(a.xf.q, -a.invI * cross(prA, P));
                a.xf.p = a.c - a.xf.q.rotate(a.localCenter);
                b.c += P * b.invMass;
                b.xf.q = integrateRot(b.xf.q, b.invI * cross(prB, P));
                b.xf.p = b.c - b.xf.q.rotate(b.localCenter);
            }
        }
        if (minSep >= -3.0f * kLinearSlop) break;
    }

    // ---- broadphase sync for everything that moved this step
    for (int32_t bi : awakeBodies_) synchronizeShapes(bi, bodies_[size_t(bi)].v * h);

    // ---- sleep: whole islands whose bodies have all been slow for timeToSleep
    stats_.sleepingIslands = 0;
    if (def_.enableSleep) {
        const float linTol2 = def_.linearSleepTolerance * def_.linearSleepTolerance;
        const float angTol = def_.angularSleepTolerance;
        for (size_t is = 0; is + 1 < island_.size(); ++is) {
            float minSleep = 1e30f;
            for (int32_t k = island_[is]; k < island_[is + 1]; ++k) {
                Body& b = bodies_[size_t(awakeBodies_[size_t(k)])];
                if (!b.allowSleep || std::fabs(b.w) > angTol || lengthSq(b.v) > linTol2) {
                    b.sleepTime = 0.0f;
                    minSleep = 0.0f;
                } else {
                    b.sleepTime += h;
                    minSleep = min(minSleep, b.sleepTime);
                }
            }
            if (minSleep < def_.timeToSleep) continue;
            ++stats_.sleepingIslands;
            for (int32_t k = island_[is]; k < island_[is + 1]; ++k) {
                Body& b = bodies_[size_t(awakeBodies_[size_t(k)])];
                b.awake = false;
                b.v = {};
                b.w = 0.0f;
                b.xf0 = b.xf;
                b.c0 = b.c;
            }
        }
    }

    int32_t awake = 0, touching = 0;
    for (int32_t bi : awakeBodies_) awake += bodies_[size_t(bi)].awake ? 1 : 0;
    for (const Contact& c : contacts_) touching += (c.alive && c.touching) ? 1 : 0;
    stats_.awakeBodies = awake;
    stats_.touchingContacts = touching;
}

// ------------------------------------------------------------------ queries

World::RayResult World::rayCastClosest(vec2 p1, vec2 p2, const Filter& filter) const {
    RayResult best;
    RayInput in{p1, p2, 1.0f};
    broadphase_.tree().rayCast(in, [&](const RayInput& clipped, int32_t proxy) -> float {
        const int32_t si = int32_t(broadphase_.tree().userData(proxy));
        const ShapeRec& s = shapes_[size_t(si)];
        if (s.sensor || !(filter.categoryBits & s.filter.maskBits) || !(s.filter.categoryBits & filter.maskBits))
            return clipped.maxFraction;
        RayHit hit;
        if (!physics2d::rayCast(s.shape, bodies_[size_t(s.body)].xf, clipped, hit)) return clipped.maxFraction;
        best.hit = true;
        best.shape = shapeIdOf(si);
        best.point = hit.point;
        best.normal = hit.normal;
        best.fraction = hit.fraction;
        return hit.fraction;
    });
    return best;
}

bool World::testPoint(vec2 p, ShapeId* out) const {
    bool found = false;
    const AABB box{p, p};
    broadphase_.tree().query(box, [&](int32_t proxy) {
        const int32_t si = int32_t(broadphase_.tree().userData(proxy));
        const ShapeRec& s = shapes_[size_t(si)];
        if (s.sensor) return true;
        const vec2 local = bodies_[size_t(s.body)].xf.invApply(p);
        bool inside;
        if (s.shape.type == ShapeType::Circle) {
            inside = lengthSq(local - s.shape.circle.center) <= s.shape.circle.radius * s.shape.circle.radius;
        } else {
            inside = true;
            const Polygon& poly = s.shape.polygon;
            for (int i = 0; i < poly.count && inside; ++i) inside = dot(poly.normals[i], local - poly.vertices[i]) <= 0.0f;
        }
        if (!inside) return true;
        found = true;
        if (out) *out = shapeIdOf(si);
        return false;
    });
    return found;
}

}  // namespace mge::physics2d
