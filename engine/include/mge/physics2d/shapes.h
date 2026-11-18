#pragma once
// physics2d shapes (M1 #18): circles, boxes and convex polygons, their mass, bounds and ray casts.
// Units are metres, kilograms and seconds; Y up, angles CCW in radians. In-house, no third-party code.
#include <cstdint>
#include "mge/math.h"

namespace mge::physics2d {

constexpr int kMaxPolygonVertices = 8;
constexpr float kLinearSlop = 0.005f;                    // allowed penetration, keeps contacts warm
constexpr float kSpeculativeDistance = 4.0f * kLinearSlop;   // contacts are made this far before touching
constexpr float kAABBMargin = 0.1f;                      // fat AABB margin in the broadphase

// Rotation as cosine/sine, so composing and applying never calls trig.
struct Rot {
    float c = 1.0f, s = 0.0f;
    static Rot fromAngle(float a) { return {std::cos(a), std::sin(a)}; }
    float angle() const { return std::atan2(s, c); }
    vec2 rotate(vec2 v) const { return {c * v.x - s * v.y, s * v.x + c * v.y}; }
    vec2 invRotate(vec2 v) const { return {c * v.x + s * v.y, -s * v.x + c * v.y}; }
};
inline Rot mul(Rot a, Rot b) { return {a.c * b.c - a.s * b.s, a.s * b.c + a.c * b.s}; }      // a then b
inline Rot invMul(Rot a, Rot b) { return {a.c * b.c + a.s * b.s, a.c * b.s - a.s * b.c}; }   // a^-1 * b

// Rigid transform: world = q * local + p.
struct Transform2 {
    vec2 p{};
    Rot q{};
    static Transform2 make(vec2 p, float angle) { return {p, Rot::fromAngle(angle)}; }
    vec2 apply(vec2 v) const { return q.rotate(v) + p; }
    vec2 invApply(vec2 v) const { return q.invRotate(v - p); }
};
// B expressed in A's frame: invMul(A, B).apply(v) == A.invApply(B.apply(v)).
inline Transform2 invMul(const Transform2& a, const Transform2& b) {
    return {a.q.invRotate(b.p - a.p), invMul(a.q, b.q)};
}

struct AABB {
    vec2 min{}, max{};
    float perimeter() const { return 2.0f * ((max.x - min.x) + (max.y - min.y)); }
    vec2 center() const { return (min + max) * 0.5f; }
    vec2 extents() const { return (max - min) * 0.5f; }
    bool contains(const AABB& o) const {
        return min.x <= o.min.x && min.y <= o.min.y && o.max.x <= max.x && o.max.y <= max.y;
    }
    bool valid() const { return max.x >= min.x && max.y >= min.y && std::isfinite(min.x) && std::isfinite(max.y); }
};
inline bool overlaps(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}
inline AABB combine(const AABB& a, const AABB& b) { return {mge::min(a.min, b.min), mge::max(a.max, b.max)}; }

// Ray from p1 towards p2; hits are reported as fractions of (p2 - p1) up to maxFraction.
struct RayInput {
    vec2 p1{}, p2{};
    float maxFraction = 1.0f;
};
struct RayHit {
    vec2 point{}, normal{};
    float fraction = 0.0f;
};
// Slab test; true when the segment touches the box at a fraction <= maxFraction.
bool rayCast(const AABB& box, const RayInput& in, float* fraction = nullptr);

struct Circle {
    vec2 center{};
    float radius = 0.5f;
};

// Convex polygon, CCW winding, no collinear vertices; normals[i] is the outward normal of
// edge vertices[i] -> vertices[(i + 1) % count].
struct Polygon {
    vec2 vertices[kMaxPolygonVertices]{};
    vec2 normals[kMaxPolygonVertices]{};
    vec2 centroid{};
    int count = 0;
};

// Axis-aligned box of half extents hx, hy centred on the origin, or placed and rotated.
Polygon makeBox(float hx, float hy);
Polygon makeBox(float hx, float hy, vec2 center, float angle);
// Convex hull of the points (up to kMaxPolygonVertices kept; welds points closer than
// kLinearSlop, drops collinear ones). Returns false for degenerate input (fewer than 3 hull
// points or near-zero area); `out` is untouched then.
bool makePolygon(const vec2* points, int count, Polygon& out);

struct MassData {
    float mass = 0.0f;
    vec2 center{};          // centre of mass, shape-local
    float inertia = 0.0f;   // rotational inertia about the centre of mass
};
MassData computeMass(const Circle& c, float density);
MassData computeMass(const Polygon& p, float density);

AABB computeAABB(const Circle& c, const Transform2& xf);
AABB computeAABB(const Polygon& p, const Transform2& xf);

// Ray casts in world space against a shape placed at xf. Rays starting inside report no hit.
bool rayCast(const Circle& c, const Transform2& xf, const RayInput& in, RayHit& hit);
bool rayCast(const Polygon& p, const Transform2& xf, const RayInput& in, RayHit& hit);

enum class ShapeType : uint8_t { Circle, Polygon };

// Tagged shape, trivially copyable, used by the broadphase/narrowphase and later by bodies.
struct Shape {
    ShapeType type;
    union {
        Circle circle;
        Polygon polygon;
    };
    Shape() : type(ShapeType::Circle), circle() {}
    Shape(const Circle& c) : type(ShapeType::Circle), circle(c) {}
    Shape(const Polygon& p) : type(ShapeType::Polygon), polygon(p) {}
};
MassData computeMass(const Shape& s, float density);
AABB computeAABB(const Shape& s, const Transform2& xf);
bool rayCast(const Shape& s, const Transform2& xf, const RayInput& in, RayHit& hit);

}  // namespace mge::physics2d
