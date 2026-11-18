#include "mge/physics2d/manifold.h"
#include <utility>

namespace mge::physics2d {

namespace {

void finish(Manifold& m, const Transform2& xfA, const Transform2& xfB) {
    for (int i = 0; i < m.count; ++i) {
        m.points[i].anchorA = m.points[i].point - xfA.p;
        m.points[i].anchorB = m.points[i].point - xfB.p;
    }
}

// Max over poly1's faces of the min signed distance of poly2's vertices to that face.
float maxSeparation(const vec2* v1, const vec2* n1, int c1, const vec2* v2, int c2, int& edge) {
    float best = -1e30f;
    edge = 0;
    for (int i = 0; i < c1; ++i) {
        float s = 1e30f;
        for (int j = 0; j < c2; ++j) s = mge::min(s, dot(n1[i], v2[j] - v1[i]));
        if (s > best) { best = s; edge = i; }
    }
    return best;
}

struct ClipVertex {
    vec2 p;
    uint32_t vertex;   // incident vertex index; +0x80 when the point was created by clipping
};

// Keeps the part of the segment with dot(n, p) <= offset. Returns the number of points out.
int clipSegment(ClipVertex out[2], const ClipVertex in[2], vec2 n, float offset) {
    int count = 0;
    const float d0 = dot(n, in[0].p) - offset, d1 = dot(n, in[1].p) - offset;
    if (d0 <= 0.0f) out[count++] = in[0];
    if (d1 <= 0.0f) out[count++] = in[1];
    if (d0 * d1 < 0.0f) {
        const float t = d0 / (d0 - d1);
        // The new point lies on the clipping plane: name it after the vertex that was cut away.
        out[count++] = {in[0].p + (in[1].p - in[0].p) * t, (d0 > 0.0f ? in[0].vertex : in[1].vertex) | 0x80u};
    }
    return count;
}

}  // namespace

Manifold collideCircles(const Circle& a, const Transform2& xfA, const Circle& b, const Transform2& xfB) {
    Manifold m;
    const vec2 ca = xfA.apply(a.center), cb = xfB.apply(b.center);
    const vec2 d = cb - ca;
    const float dist = length(d);
    const float sep = dist - a.radius - b.radius;
    if (sep > kSpeculativeDistance) return m;
    m.normal = dist > kEpsilon ? d / dist : vec2{0, 1};
    const vec2 pa = ca + m.normal * a.radius, pb = cb - m.normal * b.radius;
    m.count = 1;
    m.points[0].point = (pa + pb) * 0.5f;
    m.points[0].separation = sep;
    m.points[0].id = 0;
    finish(m, xfA, xfB);
    return m;
}

Manifold collidePolygonAndCircle(const Polygon& a, const Transform2& xfA, const Circle& b, const Transform2& xfB) {
    Manifold m;
    const vec2 c = xfA.invApply(xfB.apply(b.center));   // circle centre in A's frame
    const float r = b.radius;

    int face = 0;
    float s = -1e30f;
    for (int i = 0; i < a.count; ++i) {
        const float si = dot(a.normals[i], c - a.vertices[i]);
        if (si > s) { s = si; face = i; }
    }
    if (s - r > kSpeculativeDistance) return m;

    const vec2 v1 = a.vertices[face], v2 = a.vertices[(face + 1) % a.count];
    vec2 n, surfaceA;
    float sep;
    if (s < kEpsilon) {
        // Centre inside the polygon: push out through the least-penetrated face.
        n = a.normals[face];
        sep = s - r;
        surfaceA = c - n * s;
    } else if (dot(c - v1, v2 - v1) <= 0.0f) {          // vertex region of v1
        const float dist = length(c - v1);
        if (dist - r > kSpeculativeDistance) return m;
        n = dist > kEpsilon ? (c - v1) / dist : a.normals[face];
        sep = dist - r;
        surfaceA = v1;
    } else if (dot(c - v2, v1 - v2) <= 0.0f) {          // vertex region of v2
        const float dist = length(c - v2);
        if (dist - r > kSpeculativeDistance) return m;
        n = dist > kEpsilon ? (c - v2) / dist : a.normals[face];
        sep = dist - r;
        surfaceA = v2;
    } else {                                            // face region
        n = a.normals[face];
        sep = s - r;
        surfaceA = c - n * s;
    }
    const vec2 surfaceB = c - n * r;
    m.count = 1;
    m.normal = xfA.q.rotate(n);
    m.points[0].point = xfA.apply((surfaceA + surfaceB) * 0.5f);
    m.points[0].separation = sep;
    m.points[0].id = 0;
    finish(m, xfA, xfB);
    return m;
}

Manifold collidePolygons(const Polygon& a, const Transform2& xfA, const Polygon& b, const Transform2& xfB) {
    Manifold m;
    // Everything in A's frame.
    const Transform2 xf = invMul(xfA, xfB);
    vec2 vb[kMaxPolygonVertices], nb[kMaxPolygonVertices];
    for (int i = 0; i < b.count; ++i) {
        vb[i] = xf.apply(b.vertices[i]);
        nb[i] = xf.q.rotate(b.normals[i]);
    }
    int edgeA, edgeB;
    const float sepA = maxSeparation(a.vertices, a.normals, a.count, vb, b.count, edgeA);
    if (sepA > kSpeculativeDistance) return m;
    const float sepB = maxSeparation(vb, nb, b.count, a.vertices, a.count, edgeB);
    if (sepB > kSpeculativeDistance) return m;

    // Prefer A's face unless B's is clearly better: avoids flip-flopping between equal faces.
    const bool flip = sepB > sepA + 0.1f * kLinearSlop;
    const vec2* rv = flip ? vb : a.vertices;
    const vec2* rn = flip ? nb : a.normals;
    const int rc = flip ? b.count : a.count;
    const vec2* iv = flip ? a.vertices : vb;
    const vec2* in = flip ? a.normals : nb;
    const int ic = flip ? a.count : b.count;
    const int ref = flip ? edgeB : edgeA;

    // Incident edge: the incident polygon's face most anti-parallel to the reference normal.
    const vec2 n = rn[ref];
    int inc = 0;
    float minDot = 1e30f;
    for (int i = 0; i < ic; ++i) {
        const float d = dot(n, in[i]);
        if (d < minDot) { minDot = d; inc = i; }
    }
    const int inc2 = (inc + 1) % ic;
    const ClipVertex incident[2] = {{iv[inc], uint32_t(inc)}, {iv[inc2], uint32_t(inc2)}};

    const vec2 v1 = rv[ref], v2 = rv[(ref + 1) % rc];
    const vec2 t = normalize(v2 - v1);
    ClipVertex clip1[2], clip2[2];
    if (clipSegment(clip1, incident, -t, -dot(t, v1)) < 2) return m;
    if (clipSegment(clip2, clip1, t, dot(t, v2)) < 2) return m;

    const vec2 worldNormal = xfA.q.rotate(n);
    m.normal = flip ? -worldNormal : worldNormal;
    for (int i = 0; i < 2; ++i) {
        const float sep = dot(n, clip2[i].p - v1);
        if (sep > kSpeculativeDistance) continue;
        ManifoldPoint& mp = m.points[m.count++];
        mp.point = xfA.apply(clip2[i].p - n * (0.5f * sep));
        mp.separation = sep;
        mp.id = makeFeatureId(uint32_t(ref), clip2[i].vertex, flip);
    }
    finish(m, xfA, xfB);
    return m;
}

Manifold collide(const Shape& a, const Transform2& xfA, const Shape& b, const Transform2& xfB) {
    if (a.type == ShapeType::Circle) {
        if (b.type == ShapeType::Circle) return collideCircles(a.circle, xfA, b.circle, xfB);
        Manifold m = collidePolygonAndCircle(b.polygon, xfB, a.circle, xfA);
        m.normal = -m.normal;
        for (int i = 0; i < m.count; ++i) std::swap(m.points[i].anchorA, m.points[i].anchorB);
        return m;
    }
    if (b.type == ShapeType::Circle) return collidePolygonAndCircle(a.polygon, xfA, b.circle, xfB);
    return collidePolygons(a.polygon, xfA, b.polygon, xfB);
}

}  // namespace mge::physics2d
