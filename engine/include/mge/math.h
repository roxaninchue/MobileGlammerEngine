#pragma once
// MGE math: float-only, right-handed, column-major matrices, column vectors (v' = M * v).
// vec4/mat4/quat hot paths use NEON on arm64, SSE2 on x86_64, scalar elsewhere.
#include <cmath>
#include <cstdint>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define MGE_SIMD_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define MGE_SIMD_SSE 1
#endif

namespace mge {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-6f;

inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }
template <class T> constexpr T min(T a, T b) { return a < b ? a : b; }
template <class T> constexpr T max(T a, T b) { return a > b ? a : b; }
template <class T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- vec2
struct vec2 {
    float x = 0, y = 0;
    constexpr vec2() = default;
    constexpr vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr explicit vec2(float s) : x(s), y(s) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
constexpr vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr vec2 operator-(vec2 a) { return {-a.x, -a.y}; }
constexpr vec2 operator*(vec2 a, vec2 b) { return {a.x * b.x, a.y * b.y}; }
constexpr vec2 operator*(vec2 a, float s) { return {a.x * s, a.y * s}; }
constexpr vec2 operator*(float s, vec2 a) { return a * s; }
constexpr vec2 operator/(vec2 a, float s) { return a * (1.0f / s); }
inline vec2& operator+=(vec2& a, vec2 b) { return a = a + b; }
inline vec2& operator-=(vec2& a, vec2 b) { return a = a - b; }
inline vec2& operator*=(vec2& a, float s) { return a = a * s; }
constexpr float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr float cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }   // z of 3D cross
constexpr vec2 perp(vec2 a) { return {-a.y, a.x}; }                        // +90 degrees
inline float length(vec2 a) { return std::sqrt(dot(a, a)); }
constexpr float lengthSq(vec2 a) { return dot(a, a); }
inline vec2 normalize(vec2 a) { float l = length(a); return l > kEpsilon ? a / l : vec2{}; }
inline vec2 lerp(vec2 a, vec2 b, float t) { return a + (b - a) * t; }
inline vec2 min(vec2 a, vec2 b) { return {min(a.x, b.x), min(a.y, b.y)}; }
inline vec2 max(vec2 a, vec2 b) { return {max(a.x, b.x), max(a.y, b.y)}; }

// ---------------------------------------------------------------- vec3
struct vec3 {
    float x = 0, y = 0, z = 0;
    constexpr vec3() = default;
    constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit vec3(float s) : x(s), y(s), z(s) {}
    constexpr vec3(vec2 v, float z_) : x(v.x), y(v.y), z(z_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    constexpr vec2 xy() const { return {x, y}; }
};
constexpr vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
constexpr vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr vec3 operator*(float s, vec3 a) { return a * s; }
constexpr vec3 operator/(vec3 a, float s) { return a * (1.0f / s); }
inline vec3& operator+=(vec3& a, vec3 b) { return a = a + b; }
inline vec3& operator-=(vec3& a, vec3 b) { return a = a - b; }
inline vec3& operator*=(vec3& a, float s) { return a = a * s; }
constexpr float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr vec3 cross(vec3 a, vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(vec3 a) { return std::sqrt(dot(a, a)); }
constexpr float lengthSq(vec3 a) { return dot(a, a); }
inline vec3 normalize(vec3 a) { float l = length(a); return l > kEpsilon ? a / l : vec3{}; }
inline vec3 lerp(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline vec3 min(vec3 a, vec3 b) { return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)}; }
inline vec3 max(vec3 a, vec3 b) { return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)}; }
inline vec3 abs(vec3 a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }

// ---------------------------------------------------------------- vec4 (SIMD)
struct alignas(16) vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr vec4() = default;
    constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit vec4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr vec4(vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    constexpr vec3 xyz() const { return {x, y, z}; }
};

namespace simd {
#if MGE_SIMD_NEON
using f4 = float32x4_t;
inline f4 load(const vec4& v) { return vld1q_f32(&v.x); }
inline vec4 store(f4 r) { vec4 v; vst1q_f32(&v.x, r); return v; }
inline f4 add(f4 a, f4 b) { return vaddq_f32(a, b); }
inline f4 sub(f4 a, f4 b) { return vsubq_f32(a, b); }
inline f4 mul(f4 a, f4 b) { return vmulq_f32(a, b); }
inline f4 splat(float s) { return vdupq_n_f32(s); }
inline f4 madd(f4 acc, f4 a, f4 b) { return vfmaq_f32(acc, a, b); }   // acc + a*b
inline float hsum(f4 a) { return vaddvq_f32(a); }
#elif MGE_SIMD_SSE
using f4 = __m128;
inline f4 load(const vec4& v) { return _mm_load_ps(&v.x); }
inline vec4 store(f4 r) { vec4 v; _mm_store_ps(&v.x, r); return v; }
inline f4 add(f4 a, f4 b) { return _mm_add_ps(a, b); }
inline f4 sub(f4 a, f4 b) { return _mm_sub_ps(a, b); }
inline f4 mul(f4 a, f4 b) { return _mm_mul_ps(a, b); }
inline f4 splat(float s) { return _mm_set1_ps(s); }
inline f4 madd(f4 acc, f4 a, f4 b) { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
inline float hsum(f4 a) {
    __m128 sh = _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 s = _mm_add_ps(a, sh);
    sh = _mm_movehl_ps(sh, s);
    return _mm_cvtss_f32(_mm_add_ss(s, sh));
}
#endif
}  // namespace simd

#if MGE_SIMD_NEON || MGE_SIMD_SSE
inline vec4 operator+(vec4 a, vec4 b) { return simd::store(simd::add(simd::load(a), simd::load(b))); }
inline vec4 operator-(vec4 a, vec4 b) { return simd::store(simd::sub(simd::load(a), simd::load(b))); }
inline vec4 operator*(vec4 a, vec4 b) { return simd::store(simd::mul(simd::load(a), simd::load(b))); }
inline vec4 operator*(vec4 a, float s) { return simd::store(simd::mul(simd::load(a), simd::splat(s))); }
inline float dot(vec4 a, vec4 b) { return simd::hsum(simd::mul(simd::load(a), simd::load(b))); }
#else
inline vec4 operator+(vec4 a, vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline vec4 operator-(vec4 a, vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline vec4 operator*(vec4 a, vec4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline vec4 operator*(vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
#endif
inline vec4 operator*(float s, vec4 a) { return a * s; }
inline vec4 operator-(vec4 a) { return a * -1.0f; }
inline vec4 operator/(vec4 a, float s) { return a * (1.0f / s); }
inline vec4& operator+=(vec4& a, vec4 b) { return a = a + b; }
inline vec4& operator-=(vec4& a, vec4 b) { return a = a - b; }
inline vec4& operator*=(vec4& a, float s) { return a = a * s; }
inline float length(vec4 a) { return std::sqrt(dot(a, a)); }
inline vec4 normalize(vec4 a) { float l = length(a); return l > kEpsilon ? a / l : vec4{}; }
inline vec4 lerp(vec4 a, vec4 b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- quat (x, y, z, w)
struct alignas(16) quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr quat() = default;
    constexpr quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static quat identity() { return {}; }
    static quat axisAngle(vec3 axis, float rad) {
        vec3 n = normalize(axis);
        float s = std::sin(rad * 0.5f);
        return {n.x * s, n.y * s, n.z * s, std::cos(rad * 0.5f)};
    }
    // Euler in radians, applied Z then X then Y (yaw-pitch-roll, Y up)
    static quat euler(vec3 rad) {
        return axisAngle({0, 1, 0}, rad.y) * (axisAngle({1, 0, 0}, rad.x) * axisAngle({0, 0, 1}, rad.z));
    }
    friend quat operator*(quat a, quat b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
};
inline float dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline quat conjugate(quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline quat normalize(quat q) {
    float l = std::sqrt(dot(q, q));
    if (l < kEpsilon) return {};
    float i = 1.0f / l;
    return {q.x * i, q.y * i, q.z * i, q.w * i};
}
inline quat inverse(quat q) {
    float d = dot(q, q);
    quat c = conjugate(q);
    return {c.x / d, c.y / d, c.z / d, c.w / d};
}
// Rotate v by unit quaternion q (optimized: v + 2w(u x v) + 2u x (u x v))
inline vec3 rotate(quat q, vec3 v) {
    vec3 u{q.x, q.y, q.z};
    vec3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}
inline vec3 operator*(quat q, vec3 v) { return rotate(q, v); }
inline quat nlerp(quat a, quat b, float t) {
    if (dot(a, b) < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return normalize(quat{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)});
}
inline quat slerp(quat a, quat b, float t) {
    float d = dot(a, b);
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) return nlerp(a, b, t);
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// ---------------------------------------------------------------- mat3 (column-major)
struct mat3 {
    vec3 c[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static mat3 identity() { return {}; }
    static mat3 fromQuat(quat q) {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        mat3 m;
        m.c[0] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
        m.c[1] = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
        m.c[2] = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
        return m;
    }
    vec3& operator[](int i) { return c[i]; }
    const vec3& operator[](int i) const { return c[i]; }
};
inline vec3 operator*(const mat3& m, vec3 v) { return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z; }
inline mat3 operator*(const mat3& a, const mat3& b) {
    mat3 r;
    for (int i = 0; i < 3; ++i) r.c[i] = a * b.c[i];
    return r;
}
inline mat3 transpose(const mat3& m) {
    mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.c[i][j] = m.c[j][i];
    return r;
}
inline float determinant(const mat3& m) { return dot(m.c[0], cross(m.c[1], m.c[2])); }
inline mat3 inverse(const mat3& m) {
    vec3 r0 = cross(m.c[1], m.c[2]), r1 = cross(m.c[2], m.c[0]), r2 = cross(m.c[0], m.c[1]);
    float inv = 1.0f / dot(m.c[0], r0);
    mat3 t;  // rows r0,r1,r2 scaled -> transpose to columns
    t.c[0] = {r0.x * inv, r1.x * inv, r2.x * inv};
    t.c[1] = {r0.y * inv, r1.y * inv, r2.y * inv};
    t.c[2] = {r0.z * inv, r1.z * inv, r2.z * inv};
    return t;
}

// ---------------------------------------------------------------- mat4 (column-major, SIMD)
struct alignas(16) mat4 {
    vec4 c[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    static mat4 identity() { return {}; }
    vec4& operator[](int i) { return c[i]; }
    const vec4& operator[](int i) const { return c[i]; }
    const float* data() const { return &c[0].x; }

    static mat4 translation(vec3 t) { mat4 m; m.c[3] = {t, 1}; return m; }
    static mat4 scale(vec3 s) { mat4 m; m.c[0].x = s.x; m.c[1].y = s.y; m.c[2].z = s.z; return m; }
    static mat4 rotation(quat q) {
        mat3 r = mat3::fromQuat(q);
        mat4 m;
        for (int i = 0; i < 3; ++i) m.c[i] = {r.c[i], 0};
        return m;
    }
    // T * R * S, the common transform composition
    static mat4 trs(vec3 t, quat r, vec3 s) {
        mat3 m3 = mat3::fromQuat(r);
        mat4 m;
        m.c[0] = {m3.c[0] * s.x, 0};
        m.c[1] = {m3.c[1] * s.y, 0};
        m.c[2] = {m3.c[2] * s.z, 0};
        m.c[3] = {t, 1};
        return m;
    }
    // Vulkan clip space: depth 0..1, y down handled by the projection (flipY).
    static mat4 perspective(float fovyRad, float aspect, float zNear, float zFar, bool flipY = true) {
        float f = 1.0f / std::tan(fovyRad * 0.5f);
        mat4 m;
        m.c[0] = {f / aspect, 0, 0, 0};
        m.c[1] = {0, flipY ? -f : f, 0, 0};
        m.c[2] = {0, 0, zFar / (zNear - zFar), -1};
        m.c[3] = {0, 0, (zNear * zFar) / (zNear - zFar), 0};
        return m;
    }
    static mat4 ortho(float l, float r, float b, float t, float zNear, float zFar) {
        mat4 m;
        m.c[0] = {2 / (r - l), 0, 0, 0};
        m.c[1] = {0, 2 / (t - b), 0, 0};
        m.c[2] = {0, 0, 1 / (zNear - zFar), 0};
        m.c[3] = {-(r + l) / (r - l), -(t + b) / (t - b), zNear / (zNear - zFar), 1};
        return m;
    }
    static mat4 lookAt(vec3 eye, vec3 target, vec3 up) {
        vec3 f = normalize(target - eye), s = normalize(cross(f, up)), u = cross(s, f);
        mat4 m;
        m.c[0] = {s.x, u.x, -f.x, 0};
        m.c[1] = {s.y, u.y, -f.y, 0};
        m.c[2] = {s.z, u.z, -f.z, 0};
        m.c[3] = {-dot(s, eye), -dot(u, eye), dot(f, eye), 1};
        return m;
    }
};

inline vec4 operator*(const mat4& m, vec4 v) {
#if MGE_SIMD_NEON || MGE_SIMD_SSE
    using namespace simd;
    f4 r = mul(load(m.c[0]), splat(v.x));
    r = madd(r, load(m.c[1]), splat(v.y));
    r = madd(r, load(m.c[2]), splat(v.z));
    r = madd(r, load(m.c[3]), splat(v.w));
    return store(r);
#else
    return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z + m.c[3] * v.w;
#endif
}
inline mat4 operator*(const mat4& a, const mat4& b) {
    mat4 r;
    for (int i = 0; i < 4; ++i) r.c[i] = a * b.c[i];
    return r;
}
inline vec3 transformPoint(const mat4& m, vec3 p) { return (m * vec4(p, 1)).xyz(); }
inline vec3 transformDir(const mat4& m, vec3 d) { return (m * vec4(d, 0)).xyz(); }
inline mat4 transpose(const mat4& m) {
    mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.c[i][j] = m.c[j][i];
    return r;
}
// Fast inverse for rigid/affine transforms (rotation + translation + scale, last row 0001).
inline mat4 inverseAffine(const mat4& m) {
    mat3 l;
    for (int i = 0; i < 3; ++i) l.c[i] = m.c[i].xyz();
    mat3 il = inverse(l);
    vec3 t = -(il * m.c[3].xyz());
    mat4 r;
    for (int i = 0; i < 3; ++i) r.c[i] = {il.c[i], 0};
    r.c[3] = {t, 1};
    return r;
}
mat4 inverse(const mat4& m);   // general inverse (math.cpp)

// ---------------------------------------------------------------- geometry
struct AABB {
    vec3 min{1e30f}, max{-1e30f};   // default = empty
    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    vec3 center() const { return (min + max) * 0.5f; }
    vec3 extents() const { return (max - min) * 0.5f; }
    void expand(vec3 p) { min = mge::min(min, p); max = mge::max(max, p); }
    void expand(const AABB& b) { min = mge::min(min, b.min); max = mge::max(max, b.max); }
    float surfaceArea() const { vec3 d = max - min; return 2 * (d.x * d.y + d.y * d.z + d.z * d.x); }
};
inline bool overlaps(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
AABB transform(const AABB& box, const mat4& m);   // math.cpp

struct Ray {
    vec3 origin, dir;   // dir should be normalized
    vec3 at(float t) const { return origin + dir * t; }
};
// Slab test. Returns true and entry distance tmin on hit within [0, maxT].
bool intersect(const Ray& r, const AABB& b, float& tmin, float maxT = 1e30f);

inline bool nearlyEqual(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
inline bool nearlyEqual(vec3 a, vec3 b, float eps = 1e-4f) {
    return nearlyEqual(a.x, b.x, eps) && nearlyEqual(a.y, b.y, eps) && nearlyEqual(a.z, b.z, eps);
}

}  // namespace mge
