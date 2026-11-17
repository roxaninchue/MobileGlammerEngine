#include "test_main.h"
#include "mge/math.h"
using namespace mge;

TEST(vec3_basics) {
    vec3 a{1, 2, 3}, b{4, 5, 6};
    CHECK(dot(a, b) == 32);
    CHECK(nearlyEqual(cross(vec3{1, 0, 0}, vec3{0, 1, 0}), vec3{0, 0, 1}));
    CHECK(nearlyEqual(length(normalize(b)), 1.0f));
    CHECK(nearlyEqual(lerp(a, b, 0.5f), vec3{2.5f, 3.5f, 4.5f}));
}

TEST(vec4_simd_matches_scalar) {
    vec4 a{1, 2, 3, 4}, b{5, 6, 7, 8};
    vec4 s = a + b, m = a * b;
    CHECK(s.x == 6 && s.y == 8 && s.z == 10 && s.w == 12);
    CHECK(m.x == 5 && m.y == 12 && m.z == 21 && m.w == 32);
    CHECK(dot(a, b) == 70);
}

TEST(quat_rotation) {
    quat q = quat::axisAngle({0, 1, 0}, radians(90));
    CHECK(nearlyEqual(rotate(q, {1, 0, 0}), vec3{0, 0, -1}));
    mat3 m = mat3::fromQuat(q);
    CHECK(nearlyEqual(m * vec3{1, 0, 0}, rotate(q, {1, 0, 0})));
    quat half = slerp(quat::identity(), q, 0.5f);
    CHECK(nearlyEqual(rotate(half, {1, 0, 0}), normalize(vec3{1, 0, -1})));
    CHECK(nearlyEqual(rotate(q * inverse(q), {3, 4, 5}), vec3{3, 4, 5}));
}

TEST(mat4_trs_and_inverse) {
    mat4 m = mat4::trs({1, 2, 3}, quat::axisAngle({0, 0, 1}, radians(90)), {2, 2, 2});
    vec3 p = transformPoint(m, {1, 0, 0});
    CHECK(nearlyEqual(p, vec3{1, 4, 3}));
    mat4 inv = inverse(m), invA = inverseAffine(m);
    CHECK(nearlyEqual(transformPoint(inv, p), vec3{1, 0, 0}));
    CHECK(nearlyEqual(transformPoint(invA, p), vec3{1, 0, 0}));
    mat4 id = m * inv;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) CHECK(nearlyEqual(id[i][j], i == j ? 1.0f : 0.0f));
}

TEST(mat4_projection) {
    mat4 p = mat4::perspective(radians(60), 1.0f, 0.1f, 100.0f);
    vec4 nearPt = p * vec4{0, 0, -0.1f, 1}, farPt = p * vec4{0, 0, -100.0f, 1};
    CHECK(nearlyEqual(nearPt.z / nearPt.w, 0.0f));   // Vulkan depth 0..1
    CHECK(nearlyEqual(farPt.z / farPt.w, 1.0f));
    mat4 v = mat4::lookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    CHECK(nearlyEqual(transformPoint(v, {0, 0, 0}), vec3{0, 0, -5}));
}

TEST(aabb_and_ray) {
    AABB b{{-1, -1, -1}, {1, 1, 1}};
    float t = 0;
    CHECK(intersect(Ray{{-5, 0, 0}, {1, 0, 0}}, b, t) && nearlyEqual(t, 4.0f));
    CHECK(!intersect(Ray{{-5, 3, 0}, {1, 0, 0}}, b, t));
    AABB r = transform(b, mat4::trs({10, 0, 0}, quat::axisAngle({0, 1, 0}, radians(45)), vec3{1}));
    CHECK(nearlyEqual(r.center(), vec3{10, 0, 0}));
    CHECK(nearlyEqual(r.max.x - 10, std::sqrt(2.0f)));
    CHECK(overlaps(b, AABB{{0.5f, 0.5f, 0.5f}, {2, 2, 2}}));
}
