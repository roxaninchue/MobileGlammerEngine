#pragma once
// physics2d narrowphase (M1 #18): contact manifolds for circle and convex polygon pairs.
//  - Up to two points per manifold, each midway between the two surfaces, with its separation
//    (negative = penetrating) and a feature id that stays the same while the same features touch,
//    so the solver (#19) can warm start from last step's impulses.
//  - Contacts are made up to kSpeculativeDistance before the shapes touch, which keeps resting
//    contacts stable and stops fast bodies tunnelling through thin ones at normal speeds.
//  - Polygon vs polygon: separating axis over both polygons' face normals, then the incident edge
//    is clipped against the reference face's side planes (Sutherland-Hodgman, two points).
#include <cstdint>
#include "mge/physics2d/shapes.h"

namespace mge::physics2d {

struct ManifoldPoint {
    vec2 point{};            // world, midway between the surfaces
    vec2 anchorA{}, anchorB{};   // point relative to each body's transform origin (world-aligned)
    float separation = 0.0f; // negative when penetrating
    uint32_t id = 0;         // feature id, stable across frames for the same touching features
};

struct Manifold {
    vec2 normal{};           // world, points from shape A to shape B
    ManifoldPoint points[2];
    int count = 0;
};

// Feature ids: [flip:1][reference edge:8][incident vertex:8]; circles use 0.
constexpr uint32_t makeFeatureId(uint32_t refEdge, uint32_t incVertex, bool flip) {
    return (flip ? 0x10000u : 0u) | (refEdge << 8) | incVertex;
}

Manifold collideCircles(const Circle& a, const Transform2& xfA, const Circle& b, const Transform2& xfB);
Manifold collidePolygonAndCircle(const Polygon& a, const Transform2& xfA, const Circle& b, const Transform2& xfB);
Manifold collidePolygons(const Polygon& a, const Transform2& xfA, const Polygon& b, const Transform2& xfB);
// Any pair; a circle-polygon pair is solved as polygon-circle and the normal flipped back.
Manifold collide(const Shape& a, const Transform2& xfA, const Shape& b, const Transform2& xfB);

}  // namespace mge::physics2d
