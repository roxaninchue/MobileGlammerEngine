#include "mge/physics2d/shapes.h"
#include <algorithm>
#include "mge/assert.h"

namespace mge::physics2d {

bool rayCast(const AABB& box, const RayInput& in, float* fraction) {
    float tmin = 0.0f, tmax = in.maxFraction;
    const vec2 d = in.p2 - in.p1;
    for (int axis = 0; axis < 2; ++axis) {
        const float p = in.p1[axis], dir = d[axis];
        if (std::fabs(dir) < kEpsilon) {
            if (p < box.min[axis] || p > box.max[axis]) return false;
        } else {
            const float inv = 1.0f / dir;
            float t1 = (box.min[axis] - p) * inv, t2 = (box.max[axis] - p) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = mge::max(tmin, t1);
            tmax = mge::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    if (fraction) *fraction = tmin;
    return true;
}

static void finishPolygon(Polygon& p) {
    for (int i = 0; i < p.count; ++i) {
        const vec2 e = p.vertices[(i + 1) % p.count] - p.vertices[i];
        p.normals[i] = normalize(vec2{e.y, -e.x});   // CCW winding: outward = edge rotated -90
    }
    p.centroid = computeMass(p, 1.0f).center;
}

Polygon makeBox(float hx, float hy) {
    Polygon p;
    p.count = 4;
    p.vertices[0] = {-hx, -hy};
    p.vertices[1] = {hx, -hy};
    p.vertices[2] = {hx, hy};
    p.vertices[3] = {-hx, hy};
    p.normals[0] = {0, -1};
    p.normals[1] = {1, 0};
    p.normals[2] = {0, 1};
    p.normals[3] = {-1, 0};
    p.centroid = {};
    return p;
}

Polygon makeBox(float hx, float hy, vec2 center, float angle) {
    Polygon p = makeBox(hx, hy);
    const Transform2 xf = Transform2::make(center, angle);
    for (int i = 0; i < 4; ++i) {
        p.vertices[i] = xf.apply(p.vertices[i]);
        p.normals[i] = xf.q.rotate(p.normals[i]);
    }
    p.centroid = center;
    return p;
}

bool makePolygon(const vec2* points, int count, Polygon& out) {
    if (!points || count < 3) return false;
    // Weld near-duplicates (bounded input: we only ever look at the first 64 points).
    constexpr int kMaxInput = 64;
    vec2 pts[kMaxInput];
    int n = 0;
    for (int i = 0; i < count && n < kMaxInput; ++i) {
        bool dup = false;
        for (int j = 0; j < n && !dup; ++j) dup = lengthSq(points[i] - pts[j]) < kLinearSlop * kLinearSlop;
        if (!dup) pts[n++] = points[i];
    }
    if (n < 3) return false;

    // Andrew's monotone chain, strict turns only (drops exactly collinear points).
    std::sort(pts, pts + n, [](vec2 a, vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    vec2 hull[2 * kMaxInput];
    int h = 0;
    for (int i = 0; i < n; ++i) {
        while (h >= 2 && cross(hull[h - 1] - hull[h - 2], pts[i] - hull[h - 2]) <= 0.0f) --h;
        hull[h++] = pts[i];
    }
    for (int i = n - 2, lower = h + 1; i >= 0; --i) {
        while (h >= lower && cross(hull[h - 1] - hull[h - 2], pts[i] - hull[h - 2]) <= 0.0f) --h;
        hull[h++] = pts[i];
    }
    --h;   // last point repeats the first
    if (h < 3) return false;

    auto removeAt = [&](int i) { for (int k = i; k + 1 < h; ++k) hull[k] = hull[k + 1]; --h; };
    // Drop nearly collinear vertices (closer than the slop to the line through their neighbours).
    for (bool changed = true; changed && h > 3;) {
        changed = false;
        for (int i = 0; i < h && h > 3; ++i) {
            const vec2 a = hull[(i + h - 1) % h], b = hull[i], c = hull[(i + 1) % h];
            const float len = length(c - a);
            if (len > kEpsilon && std::fabs(cross(c - a, b - a)) / len < kLinearSlop) { removeAt(i); changed = true; }
        }
    }
    // Too many vertices: repeatedly drop the one whose removal loses the least area.
    while (h > kMaxPolygonVertices) {
        int best = 0;
        float bestArea = 1e30f;
        for (int i = 0; i < h; ++i) {
            const float area = cross(hull[i] - hull[(i + h - 1) % h], hull[(i + 1) % h] - hull[(i + h - 1) % h]);
            if (area < bestArea) { bestArea = area; best = i; }
        }
        removeAt(best);
    }
    float area2 = 0.0f;
    for (int i = 0; i < h; ++i) area2 += cross(hull[i], hull[(i + 1) % h]);
    if (h < 3 || area2 * 0.5f < kLinearSlop * kLinearSlop) return false;

    Polygon p;
    p.count = h;
    for (int i = 0; i < h; ++i) p.vertices[i] = hull[i];
    finishPolygon(p);
    out = p;
    return true;
}

MassData computeMass(const Circle& c, float density) {
    MassData m;
    const float rr = c.radius * c.radius;
    m.mass = density * kPi * rr;
    m.center = c.center;
    m.inertia = m.mass * 0.5f * rr;   // solid disc about its own centre
    return m;
}

MassData computeMass(const Polygon& p, float density) {
    MassData m;
    if (p.count < 3) return m;
    // Fan of triangles from the first vertex (keeps the numbers small for far-from-origin shapes).
    const vec2 s = p.vertices[0];
    float area = 0.0f, I = 0.0f;
    vec2 center{};
    for (int i = 1; i + 1 < p.count; ++i) {
        const vec2 e1 = p.vertices[i] - s, e2 = p.vertices[i + 1] - s;
        const float D = cross(e1, e2);
        const float triArea = 0.5f * D;
        area += triArea;
        center += (e1 + e2) * (triArea / 3.0f);
        const float ix2 = e1.x * e1.x + e2.x * e1.x + e2.x * e2.x;
        const float iy2 = e1.y * e1.y + e2.y * e1.y + e2.y * e2.y;
        I += (0.25f / 3.0f) * D * (ix2 + iy2);
    }
    if (area <= kEpsilon) return m;
    center = center / area;                 // relative to s
    m.mass = density * area;
    m.center = center + s;
    m.inertia = density * I - m.mass * dot(center, center);   // shift from s to the centroid
    return m;
}

MassData computeMass(const Shape& s, float density) {
    return s.type == ShapeType::Circle ? computeMass(s.circle, density) : computeMass(s.polygon, density);
}

AABB computeAABB(const Circle& c, const Transform2& xf) {
    const vec2 p = xf.apply(c.center);
    return {p - vec2{c.radius}, p + vec2{c.radius}};
}

AABB computeAABB(const Polygon& p, const Transform2& xf) {
    vec2 lo = xf.apply(p.vertices[0]), hi = lo;
    for (int i = 1; i < p.count; ++i) {
        const vec2 v = xf.apply(p.vertices[i]);
        lo = mge::min(lo, v);
        hi = mge::max(hi, v);
    }
    return {lo, hi};
}

AABB computeAABB(const Shape& s, const Transform2& xf) {
    return s.type == ShapeType::Circle ? computeAABB(s.circle, xf) : computeAABB(s.polygon, xf);
}

bool rayCast(const Circle& c, const Transform2& xf, const RayInput& in, RayHit& hit) {
    const vec2 center = xf.apply(c.center);
    const vec2 s = in.p1 - center, d = in.p2 - in.p1;
    const float cc = dot(s, s) - c.radius * c.radius;
    if (cc < 0.0f) return false;   // starts inside
    const float a = dot(d, d), b = dot(s, d);
    const float disc = b * b - a * cc;
    if (a < kEpsilon || disc < 0.0f) return false;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t < 0.0f || t > in.maxFraction) return false;
    hit.fraction = t;
    hit.point = in.p1 + d * t;
    hit.normal = normalize(hit.point - center);
    return true;
}

bool rayCast(const Polygon& p, const Transform2& xf, const RayInput& in, RayHit& hit) {
    // Clip the segment against every edge's half plane, in the polygon's frame.
    const vec2 p1 = xf.invApply(in.p1), d = xf.q.invRotate(in.p2 - in.p1);
    float lower = 0.0f, upper = in.maxFraction;
    int index = -1;
    for (int i = 0; i < p.count; ++i) {
        const float num = dot(p.normals[i], p.vertices[i] - p1);
        const float den = dot(p.normals[i], d);
        if (den == 0.0f) {
            if (num < 0.0f) return false;   // parallel and outside this edge
        } else if (den < 0.0f && num < lower * den) {
            lower = num / den;              // entering
            index = i;
        } else if (den > 0.0f && num < upper * den) {
            upper = num / den;              // leaving
        }
        if (upper < lower) return false;
    }
    if (index < 0) return false;            // started inside
    hit.fraction = lower;
    hit.point = in.p1 + (in.p2 - in.p1) * lower;
    hit.normal = xf.q.rotate(p.normals[index]);
    return true;
}

bool rayCast(const Shape& s, const Transform2& xf, const RayInput& in, RayHit& hit) {
    return s.type == ShapeType::Circle ? rayCast(s.circle, xf, in, hit) : rayCast(s.polygon, xf, in, hit);
}

}  // namespace mge::physics2d
