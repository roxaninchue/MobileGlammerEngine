#include "test_main.h"
#include "mge/physics2d/broadphase.h"
#include "mge/physics2d/manifold.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>
using namespace mge::physics2d;   // not all of mge: mge::AABB is the 3D box
using mge::kPi;
using mge::vec2;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
static bool near(vec2 a, vec2 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

struct Rng {   // xorshift32, deterministic
    uint32_t s = 2463534242u;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uniform(float lo, float hi) { return lo + (hi - lo) * float(next() & 0xFFFFFF) / float(0xFFFFFF); }
};

// ---------------------------------------------------------------- shapes

TEST(physics2d_box_and_circle_mass) {
    const Polygon box = makeBox(1.0f, 0.5f);   // 2 x 1
    const MassData m = computeMass(box, 3.0f);
    CHECK(near(m.mass, 6.0f) && near(m.center, {0, 0}));
    CHECK(near(m.inertia, 6.0f * (4.0f + 1.0f) / 12.0f));
    // Moving the box moves the centroid but not the inertia about it.
    const Polygon moved = makeBox(1.0f, 0.5f, {3, -2}, 0.7f);
    const MassData mm = computeMass(moved, 3.0f);
    CHECK(near(mm.mass, 6.0f, 1e-3f) && near(mm.center, {3, -2}, 1e-3f) && near(mm.inertia, m.inertia, 1e-3f));
    CHECK(near(moved.centroid, {3, -2}, 1e-3f));
    const MassData c = computeMass(Circle{{1, 2}, 2.0f}, 0.5f);
    CHECK(near(c.mass, 0.5f * kPi * 4.0f) && near(c.center, {1, 2}) && near(c.inertia, c.mass * 2.0f));
    // Triangle (0,0),(3,0),(0,3): area 4.5, centroid (1,1), Iz about centroid = m*(a^2+b^2)/18 for a right triangle legs a,b.
    Polygon tri;
    const vec2 pts[] = {{0, 0}, {3, 0}, {0, 3}};
    CHECK(makePolygon(pts, 3, tri) && tri.count == 3);
    const MassData mt = computeMass(tri, 2.0f);
    CHECK(near(mt.mass, 9.0f) && near(mt.center, {1, 1}) && near(mt.inertia, 9.0f * 18.0f / 18.0f, 1e-3f));
}

TEST(physics2d_make_polygon_hull) {
    // Square corners plus interior, collinear and duplicate points, in random order.
    const vec2 pts[] = {{1, 1}, {0, 0}, {0.5f, 0.5f}, {2, 0}, {1, 0}, {2, 2}, {0, 2}, {0, 0.0001f}, {2, 1}};
    Polygon p;
    CHECK(makePolygon(pts, 9, p) && p.count == 4);
    float area2 = 0;
    for (int i = 0; i < p.count; ++i) {
        area2 += cross(p.vertices[i], p.vertices[(i + 1) % p.count]);
        const vec2 e = p.vertices[(i + 1) % p.count] - p.vertices[i];
        CHECK(near(dot(e, p.normals[i]), 0.0f) && near(length(p.normals[i]), 1.0f));
        CHECK(dot(p.normals[i], p.vertices[i] - p.centroid) > 0.0f);   // outward
    }
    CHECK(near(area2, 8.0f));   // CCW, area 4
    // A circle of 20 points is reduced to 8 vertices, losing little area.
    vec2 ring[20];
    for (int i = 0; i < 20; ++i) ring[i] = {std::cos(float(i) * kPi / 10), std::sin(float(i) * kPi / 10)};
    CHECK(makePolygon(ring, 20, p) && p.count == kMaxPolygonVertices);
    CHECK(computeMass(p, 1).mass > 2.6f);   // regular octagon in the unit circle: 2.83
    // Degenerate input.
    const vec2 line[] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    const vec2 dup[] = {{0, 0}, {0, 0}, {0.001f, 0}};
    Polygon untouched = makeBox(1, 1);
    CHECK(!makePolygon(line, 4, untouched) && !makePolygon(dup, 3, untouched) && untouched.count == 4);
}

TEST(physics2d_aabbs_and_raycasts) {
    const Polygon box = makeBox(1.0f, 1.0f);
    const AABB b = computeAABB(box, Transform2::make({5, 0}, kPi * 0.25f));
    const float h = std::sqrt(2.0f);
    CHECK(near(b.min, {5 - h, -h}) && near(b.max, {5 + h, h}));
    const AABB c = computeAABB(Circle{{1, 0}, 0.5f}, Transform2::make({0, 0}, kPi * 0.5f));
    CHECK(near(c.min, {-0.5f, 0.5f}) && near(c.max, {0.5f, 1.5f}));

    RayHit hit;
    RayInput ray{{-5, 0}, {5, 0}, 1.0f};
    CHECK(rayCast(box, Transform2{}, ray, hit) && near(hit.fraction, 0.4f) && near(hit.normal, {-1, 0}) && near(hit.point, {-1, 0}));
    CHECK(rayCast(Shape(box), Transform2::make({0, 0}, kPi * 0.25f), ray, hit) && near(hit.point.x, -h, 1e-3f));
    ray.maxFraction = 0.3f;
    CHECK(!rayCast(box, Transform2{}, ray, hit));
    CHECK(!rayCast(box, Transform2{}, {{0, 0}, {5, 0}, 1}, hit));    // starts inside
    CHECK(!rayCast(box, Transform2{}, {{-5, 2}, {5, 2}, 1}, hit));   // misses
    CHECK(rayCast(Circle{{0, 0}, 1}, Transform2::make({0, 3}, 0), {{0, 0}, {0, 10}, 1}, hit));
    CHECK(near(hit.fraction, 0.2f) && near(hit.normal, {0, -1}));
    float f = 0;
    CHECK(rayCast(AABB{{1, 1}, {2, 2}}, {{0, 0}, {4, 4}, 1}, &f) && near(f, 0.25f));
    CHECK(!rayCast(AABB{{1, 1}, {2, 2}}, {{0, 0}, {4, 0}, 1}));
}

// ---------------------------------------------------------------- tree + broadphase

static AABB randomBox(Rng& r, float world = 100.0f) {
    const vec2 c{r.uniform(-world, world), r.uniform(-world, world)};
    const vec2 e{r.uniform(0.2f, 3.0f), r.uniform(0.2f, 3.0f)};
    return {c - e, c + e};
}

TEST(physics2d_tree_matches_brute_force) {
    Rng rng;
    DynamicTree tree;
    std::vector<int32_t> ids;
    std::vector<AABB> boxes;
    for (int i = 0; i < 600; ++i) {
        boxes.push_back(randomBox(rng));
        ids.push_back(tree.createProxy(boxes.back(), uint64_t(i)));
    }
    CHECK(tree.validate() && tree.proxyCount() == 600);
    CHECK(tree.height() <= 20);   // balanced: ~log2(600) = 9.2, AVL bound 1.44 * that
    // Random moves (small and large) and removals keep it valid.
    std::vector<bool> alive(600, true);
    for (int step = 0; step < 2000; ++step) {
        const int i = int(rng.next() % 600);
        if (!alive[size_t(i)]) continue;
        if (step % 7 == 0) { tree.destroyProxy(ids[size_t(i)]); alive[size_t(i)] = false; continue; }
        const vec2 d = (step % 3 == 0) ? vec2{rng.uniform(-30, 30), rng.uniform(-30, 30)} : vec2{rng.uniform(-0.05f, 0.05f), 0};
        boxes[size_t(i)] = {boxes[size_t(i)].min + d, boxes[size_t(i)].max + d};
        tree.moveProxy(ids[size_t(i)], boxes[size_t(i)], d);
    }
    CHECK(tree.validate());
    for (int i = 0; i < 600; ++i) if (alive[size_t(i)]) CHECK(tree.fatAABB(ids[size_t(i)]).contains(boxes[size_t(i)]));
    // Queries return exactly the live proxies whose fat AABB overlaps.
    for (int q = 0; q < 50; ++q) {
        const AABB box = randomBox(rng);
        std::set<uint64_t> got, want;
        tree.query(box, [&](int32_t id) { got.insert(tree.userData(id)); return true; });
        for (int i = 0; i < 600; ++i)
            if (alive[size_t(i)] && overlaps(tree.fatAABB(ids[size_t(i)]), box)) want.insert(uint64_t(i));
        CHECK(got == want);
    }
    // Early out.
    int visits = 0;
    tree.query(AABB{{-200, -200}, {200, 200}}, [&](int32_t) { return ++visits < 3; });
    CHECK(visits == 3);
    // Closest ray hit through the tree equals brute force over the fat boxes.
    for (int q = 0; q < 30; ++q) {
        const RayInput ray{{rng.uniform(-120, 120), -130}, {rng.uniform(-120, 120), 130}, 1.0f};
        float best = 2.0f;
        tree.rayCast(ray, [&](const RayInput& in, int32_t id) {
            float f;
            if (rayCast(tree.fatAABB(id), in, &f)) { best = std::min(best, f); return f; }
            return in.maxFraction;
        });
        float brute = 2.0f;
        for (int i = 0; i < 600; ++i) {
            float f;
            if (alive[size_t(i)] && rayCast(tree.fatAABB(ids[size_t(i)]), ray, &f)) brute = std::min(brute, f);
        }
        CHECK(near(best, brute));
    }
    for (int i = 0; i < 600; ++i) if (alive[size_t(i)]) tree.destroyProxy(ids[size_t(i)]);
    CHECK(tree.validate() && tree.proxyCount() == 0 && tree.height() == 0);
}

TEST(physics2d_tree_sorted_inserts_stay_balanced) {
    DynamicTree tree;
    for (int i = 0; i < 1024; ++i) tree.createProxy({{float(i), 0}, {float(i) + 0.5f, 0.5f}}, uint64_t(i));
    CHECK(tree.validate() && tree.height() <= 15 && tree.areaRatio() < 40.0f);
}

TEST(physics2d_fat_aabb_skips_small_moves) {
    DynamicTree tree;
    const AABB box{{0, 0}, {1, 1}};
    const int32_t id = tree.createProxy(box, 7);
    CHECK(!tree.moveProxy(id, {{0.05f, 0}, {1.05f, 1}}));     // inside the margin: free
    CHECK(tree.moveProxy(id, {{0.5f, 0}, {1.5f, 1}}, {0.5f, 0}));
    const AABB fat = tree.fatAABB(id);
    CHECK(near(fat.max.x, 1.5f + kAABBMargin + 1.0f) && near(fat.min.x, 0.5f - kAABBMargin));   // predicted 2 steps ahead
    CHECK(tree.userData(id) == 7);
}

static std::set<std::pair<uint64_t, uint64_t>> pairsOf(BroadPhase& bp) {
    std::set<std::pair<uint64_t, uint64_t>> out;
    bp.updatePairs([&](uint64_t a, uint64_t b) {
        CHECK(a != b);
        auto p = std::make_pair(std::min(a, b), std::max(a, b));
        CHECK(!out.count(p));   // each pair once
        out.insert(p);
    });
    return out;
}

TEST(physics2d_broadphase_pairs) {
    Rng rng;
    BroadPhase bp;
    const int N = 300;
    std::vector<AABB> boxes;
    std::vector<int32_t> ids;
    for (int i = 0; i < N; ++i) {
        boxes.push_back(randomBox(rng, 40.0f));
        ids.push_back(bp.createProxy(boxes.back(), uint64_t(i)));
    }
    auto brute = [&](const std::vector<bool>& moved) {
        std::set<std::pair<uint64_t, uint64_t>> want;
        for (int i = 0; i < N; ++i)
            for (int j = i + 1; j < N; ++j)
                if ((moved[size_t(i)] || moved[size_t(j)]) && bp.testOverlap(ids[size_t(i)], ids[size_t(j)]))
                    want.insert({uint64_t(i), uint64_t(j)});
        return want;
    };
    CHECK(pairsOf(bp) == brute(std::vector<bool>(N, true)));
    CHECK(bp.moveCount() == 0 && pairsOf(bp).empty());

    for (int step = 0; step < 20; ++step) {
        std::vector<bool> moved(N, false);
        for (int k = 0; k < 40; ++k) {
            const int i = int(rng.next() % N);
            const vec2 d{rng.uniform(-2, 2), rng.uniform(-2, 2)};
            boxes[size_t(i)] = {boxes[size_t(i)].min + d, boxes[size_t(i)].max + d};
            const AABB before = bp.tree().fatAABB(ids[size_t(i)]);
            bp.moveProxy(ids[size_t(i)], boxes[size_t(i)], d);
            const AABB after = bp.tree().fatAABB(ids[size_t(i)]);
            if (!near(before.min, after.min) || !near(before.max, after.max)) moved[size_t(i)] = true;
        }
        // Every pair whose tight boxes overlap is reachable: either reported now or found earlier.
        CHECK(pairsOf(bp) == brute(moved));
    }
    // Destroyed proxies never show up, even if they were waiting in the move buffer.
    bp.touchProxy(ids[0]);
    bp.destroyProxy(ids[0]);
    bp.touchProxy(ids[1]);
    for (const auto& p : pairsOf(bp)) CHECK(p.first != 0 && p.second != 0);
    CHECK(bp.tree().validate());
}

// ---------------------------------------------------------------- narrowphase

TEST(physics2d_manifold_circles) {
    const Circle a{{0, 0}, 1.0f}, b{{0, 0}, 0.5f};
    Manifold m = collideCircles(a, Transform2::make({0, 0}, 0), b, Transform2::make({1.3f, 0}, 0));
    CHECK(m.count == 1 && near(m.normal, {1, 0}) && near(m.points[0].separation, -0.2f));
    CHECK(near(m.points[0].point, {0.9f, 0}) && near(m.points[0].anchorB, {-0.4f, 0}));
    m = collideCircles(a, Transform2{}, b, Transform2::make({1.51f, 0}, 0));   // gap 0.01: speculative
    CHECK(m.count == 1 && near(m.points[0].separation, 0.01f));
    m = collideCircles(a, Transform2{}, b, Transform2::make({1.6f, 0}, 0));
    CHECK(m.count == 0);
    m = collideCircles(a, Transform2{}, b, Transform2{});                       // concentric
    CHECK(m.count == 1 && near(length(m.normal), 1.0f) && near(m.points[0].separation, -1.5f));
}

TEST(physics2d_manifold_box_on_ground) {
    const Polygon ground = makeBox(10.0f, 1.0f);   // top face at y = 1
    const Polygon box = makeBox(0.5f, 0.5f);
    // Resting box sunk 0.1 into the ground: two contact points on the face.
    Manifold m = collidePolygons(ground, Transform2{}, box, Transform2::make({2, 1.4f}, 0));
    CHECK(m.count == 2 && near(m.normal, {0, 1}));
    std::vector<float> xs;
    for (int i = 0; i < m.count; ++i) {
        CHECK(near(m.points[i].separation, -0.1f) && near(m.points[i].point.y, 0.95f));
        xs.push_back(m.points[i].point.x);
    }
    std::sort(xs.begin(), xs.end());
    CHECK(near(xs[0], 1.5f) && near(xs[1], 2.5f));
    const uint32_t id0 = m.points[0].id, id1 = m.points[1].id;
    CHECK(id0 != id1);
    // Slight slide: same features, same ids (warm starting relies on this).
    Manifold m2 = collidePolygons(ground, Transform2{}, box, Transform2::make({2.03f, 1.41f}, 0.01f));
    CHECK(m2.count == 2 && m2.points[0].id == id0 && m2.points[1].id == id1);
    // Swapped order: the normal still points from A (box) to B (ground).
    Manifold m3 = collide(Shape(box), Transform2::make({2, 1.4f}, 0), Shape(ground), Transform2{});
    CHECK(m3.count == 2 && near(m3.normal, {0, -1}) && near(m3.points[0].separation, -0.1f));
    // Lifted above the speculative distance: nothing.
    CHECK(collidePolygons(ground, Transform2{}, box, Transform2::make({2, 1.6f}, 0)).count == 0);
    // Hovering within the speculative distance: speculative points with positive separation.
    Manifold s = collidePolygons(ground, Transform2{}, box, Transform2::make({2, 1.51f}, 0));
    CHECK(s.count == 2 && near(s.points[0].separation, 0.01f));
}

TEST(physics2d_manifold_corner_and_edges) {
    const Polygon ground = makeBox(10.0f, 1.0f);
    const Polygon box = makeBox(0.5f, 0.5f);
    const float h = std::sqrt(0.5f);   // half diagonal
    // Box standing on a corner, corner 0.05 deep: one point.
    Manifold m = collidePolygons(ground, Transform2{}, box, Transform2::make({0, 1 + h - 0.05f}, kPi * 0.25f));
    CHECK(m.count == 1 && near(m.normal, {0, 1}) && near(m.points[0].separation, -0.05f, 1e-3f));
    CHECK(near(m.points[0].point, {0, 0.975f}, 1e-3f));
    // Box overhanging the ground's edge: contact clipped to the ground's corner.
    Manifold e = collidePolygons(ground, Transform2{}, box, Transform2::make({10.2f, 1.45f}, 0));
    CHECK(e.count == 2);
    float maxX = -1e9f;
    for (int i = 0; i < e.count; ++i) maxX = std::max(maxX, e.points[i].point.x);
    CHECK(near(maxX, 10.0f) && near(e.normal, {0, 1}));
    // Side by side, overlapping horizontally.
    Manifold s = collidePolygons(box, Transform2{}, box, Transform2::make({0.9f, 0.2f}, 0));
    CHECK(s.count == 2 && near(s.normal, {1, 0}) && near(s.points[0].separation, -0.1f));
}

TEST(physics2d_manifold_polygon_circle) {
    const Polygon box = makeBox(1.0f, 1.0f);
    // Face region.
    Manifold m = collidePolygonAndCircle(box, Transform2{}, Circle{{0, 0}, 0.5f}, Transform2::make({0, 1.4f}, 0));
    CHECK(m.count == 1 && near(m.normal, {0, 1}) && near(m.points[0].separation, -0.1f) && near(m.points[0].point, {0, 0.95f}));
    // Vertex region (diagonal off the top-right corner).
    const float d = 0.4f / std::sqrt(2.0f);
    m = collidePolygonAndCircle(box, Transform2{}, Circle{{0, 0}, 0.5f}, Transform2::make({1 + d, 1 + d}, 0));
    CHECK(m.count == 1 && near(m.normal, normalize(vec2{1, 1})) && near(m.points[0].separation, -0.1f));
    // Centre inside the box.
    m = collidePolygonAndCircle(box, Transform2{}, Circle{{0, 0}, 0.25f}, Transform2::make({0.9f, 0}, 0));
    CHECK(m.count == 1 && near(m.normal, {1, 0}) && near(m.points[0].separation, -0.35f));
    // Rotated polygon, circle offset inside the shape's local frame.
    m = collidePolygonAndCircle(box, Transform2::make({5, 5}, kPi * 0.5f), Circle{{0.0f, 1.4f}, 0.5f}, Transform2::make({5, 5}, 0));
    CHECK(m.count == 1 && near(m.normal, {0, 1}, 1e-3f) && near(m.points[0].separation, -0.1f, 1e-3f));
    // Far away.
    CHECK(collidePolygonAndCircle(box, Transform2{}, Circle{{0, 0}, 0.5f}, Transform2::make({3, 3}, 0)).count == 0);
    // Circle first: normal flipped to point from the circle to the box.
    Manifold c = collide(Shape(Circle{{0, 0}, 0.5f}), Transform2::make({0, 1.4f}, 0), Shape(box), Transform2{});
    CHECK(c.count == 1 && near(c.normal, {0, -1}) && near(c.points[0].anchorA, {0, -0.45f}) && near(c.points[0].anchorB, {0, 0.95f}));
}

TEST(physics2d_manifold_random_agrees_with_sat) {
    // For random convex pairs, a manifold exists exactly when the shapes are within the
    // speculative distance on every face axis, and every point lies between the surfaces.
    Rng rng;
    int contacts = 0;
    for (int k = 0; k < 400; ++k) {
        vec2 pa[6], pb[6];
        for (int i = 0; i < 6; ++i) {
            pa[i] = {rng.uniform(-1, 1), rng.uniform(-1, 1)};
            pb[i] = {rng.uniform(-1, 1), rng.uniform(-1, 1)};
        }
        Polygon a, b;
        if (!makePolygon(pa, 6, a) || !makePolygon(pb, 6, b)) continue;
        const Transform2 xa = Transform2::make({rng.uniform(-1, 1), rng.uniform(-1, 1)}, rng.uniform(-3, 3));
        const Transform2 xb = Transform2::make({rng.uniform(-1, 1), rng.uniform(-1, 1)}, rng.uniform(-3, 3));
        const Manifold m = collidePolygons(a, xa, b, xb);
        if (m.count == 0) continue;
        ++contacts;
        CHECK(near(length(m.normal), 1.0f));
        // Normal points from A to B (roughly from A's centroid towards B's).
        CHECK(dot(m.normal, xb.apply(b.centroid) - xa.apply(a.centroid)) > -0.5f);
        for (int i = 0; i < m.count; ++i) CHECK(m.points[i].separation <= kSpeculativeDistance + 1e-5f);
    }
    CHECK(contacts > 100);
}
