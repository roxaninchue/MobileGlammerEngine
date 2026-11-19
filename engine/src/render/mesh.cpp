#include "mge/render/mesh.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include "mge/serialize.h"
#include "mge/transform3d.h"

namespace mge::render {

void MeshData::computeBounds() {
    bounds = AABB{};
    for (const Vertex3D& v : vertices) bounds.expand(v.position);
}

void MeshData::computeNormals() {
    std::vector<vec3> acc(vertices.size(), vec3{});
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        // Unnormalised cross product: its length is twice the area, so big faces weigh more.
        const vec3 n = cross(vertices[b].position - vertices[a].position, vertices[c].position - vertices[a].position);
        acc[a] += n; acc[b] += n; acc[c] += n;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        const vec3 n = normalize(acc[i]);
        vertices[i].normal = length(n) > 0.5f ? n : vec3{0, 1, 0};
    }
}

void MeshData::computeTangents() {
    std::vector<vec3> tan(vertices.size(), vec3{}), bit(vertices.size(), vec3{});
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        const Vertex3D &a = vertices[ia], &b = vertices[ib], &c = vertices[ic];
        const vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        const vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;   // degenerate uvs: no tangent from this face
        const float r = 1.0f / det;
        const vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {ia, ib, ic}) { tan[k] += t; bit[k] += bt; }
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        const vec3 n = vertices[i].normal;
        vec3 t = tan[i] - n * dot(n, tan[i]);   // Gram-Schmidt
        if (length(t) < 1e-8f) {
            // No uv gradient: any vector perpendicular to the normal.
            t = std::fabs(n.x) < 0.9f ? cross(n, vec3{1, 0, 0}) : cross(n, vec3{0, 1, 0});
            t = cross(t, n);
        }
        t = normalize(t);
        const float w = dot(cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        vertices[i].tangent = {t, w};
    }
}

std::vector<uint16_t> MeshData::indices16() const {
    std::vector<uint16_t> out(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) out[i] = uint16_t(indices[i]);
    return out;
}

bool MeshData::validate(std::string* error) const {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (indices.size() % 3) return fail("index count is not a multiple of 3");
    for (uint32_t i : indices) if (i >= vertices.size()) return fail("index out of range");
    for (const Vertex3D& v : vertices)
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z))
            return fail("non-finite position");
    return true;
}

// ------------------------------------------------------------------ primitives
namespace primitives {
namespace {

// A surface of revolution around Y: rows top to bottom, each a ring of segments + 1 vertices
// (the seam is duplicated for uvs). Row r is at polar angle phi[r] on a sphere of `radius`,
// lifted by yOffset[r]. Normals are the sphere's. Degenerate pole triangles are skipped.
MeshData lathe(float radius, uint32_t segments, const std::vector<float>& phi, const std::vector<float>& yOffset) {
    MeshData m;
    const uint32_t rows = uint32_t(phi.size());
    float totalH = 0;
    std::vector<float> ys(rows);
    for (uint32_t r = 0; r < rows; ++r) ys[r] = std::cos(phi[r]) * radius + yOffset[r];
    totalH = ys.front() - ys.back();
    m.vertices.reserve(size_t(rows) * (segments + 1));
    for (uint32_t r = 0; r < rows; ++r) {
        const float sp = std::sin(phi[r]), cp = std::cos(phi[r]);
        for (uint32_t s = 0; s <= segments; ++s) {
            const float th = 2.0f * kPi * float(s) / float(segments);
            const vec3 n{sp * std::sin(th), cp, sp * std::cos(th)};
            Vertex3D v;
            v.normal = n;
            v.position = {n.x * radius, ys[r], n.z * radius};
            v.uv = {float(s) / float(segments), totalH > 0 ? (ys.front() - ys[r]) / totalH : 0.0f};
            m.vertices.push_back(v);
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r + 1 < rows; ++r) {
        const bool topPole = std::sin(phi[r]) < 1e-6f, bottomPole = std::sin(phi[r + 1]) < 1e-6f;
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = r * stride + s, b = a + stride;   // a above b
            if (!topPole) m.indices.insert(m.indices.end(), {a, b, a + 1});
            if (!bottomPole) m.indices.insert(m.indices.end(), {a + 1, b, b + 1});
        }
    }
    return m;
}

void finish(MeshData& m) {
    m.computeTangents();
    m.computeBounds();
}

}  // namespace

MeshData cube(vec3 size) {
    struct Face { vec3 n, u, v; };   // u x v = n: counter-clockwise seen from outside
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    const vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    MeshData m;
    for (const Face& f : faces) {
        const uint32_t base = uint32_t(m.vertices.size());
        for (const vec2& c : corners) {
            Vertex3D v;
            const vec3 p = (f.n + f.u * c.x + f.v * c.y) * 0.5f;
            v.position = {p.x * size.x, p.y * size.y, p.z * size.z};
            v.normal = f.n;
            v.uv = {(c.x + 1) * 0.5f, 1 - (c.y + 1) * 0.5f};   // texture top along +v
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    finish(m);
    return m;
}

MeshData plane(vec2 size, uint32_t subdivisions) {
    const uint32_t n = std::max(1u, subdivisions);
    MeshData m;
    for (uint32_t j = 0; j <= n; ++j)
        for (uint32_t i = 0; i <= n; ++i) {
            Vertex3D v;
            const float u = float(i) / float(n), w = float(j) / float(n);
            v.position = {(u - 0.5f) * size.x, 0, (w - 0.5f) * size.y};
            v.normal = {0, 1, 0};
            v.uv = {u, w};
            m.vertices.push_back(v);
        }
    const uint32_t stride = n + 1;
    for (uint32_t j = 0; j < n; ++j)
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t a = j * stride + i, b = a + 1, d = a + stride, c = d + 1;   // d, c one row further in +Z
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b});
        }
    finish(m);
    return m;
}

MeshData sphere(float radius, uint32_t segments, uint32_t rings) {
    segments = std::max(3u, segments);
    rings = std::max(2u, rings);
    std::vector<float> phi(rings + 1), off(rings + 1, 0.0f);
    for (uint32_t r = 0; r <= rings; ++r) phi[r] = kPi * float(r) / float(rings);
    MeshData m = lathe(radius, segments, phi, off);
    finish(m);
    return m;
}

MeshData capsule(float radius, float height, uint32_t segments, uint32_t rings) {
    segments = std::max(3u, segments);
    rings = std::max(1u, rings);
    const float half = std::max(0.0f, height * 0.5f - radius);   // half the straight part
    std::vector<float> phi, off;
    for (uint32_t r = 0; r <= rings; ++r) { phi.push_back(0.5f * kPi * float(r) / float(rings)); off.push_back(half); }
    for (uint32_t r = 0; r <= rings; ++r) { phi.push_back(0.5f * kPi * (1 + float(r) / float(rings))); off.push_back(-half); }
    MeshData m = lathe(radius, segments, phi, off);
    finish(m);
    return m;
}

MeshData cylinder(float radius, float height, uint32_t segments) {
    segments = std::max(3u, segments);
    const float h = height * 0.5f;
    // Side: two rings with radial normals (a lathe at the equator, lifted to each end).
    MeshData m = lathe(radius, segments, {0.5f * kPi, 0.5f * kPi}, {h, -h});
    // Caps: a centre and a ring each, facing +Y / -Y.
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? h : -h, ny = cap == 0 ? 1.0f : -1.0f;
        const uint32_t centre = uint32_t(m.vertices.size());
        Vertex3D c;
        c.position = {0, y, 0};
        c.normal = {0, ny, 0};
        c.uv = {0.5f, 0.5f};
        m.vertices.push_back(c);
        for (uint32_t s = 0; s <= segments; ++s) {
            const float th = 2.0f * kPi * float(s) / float(segments);
            Vertex3D v;
            v.position = {std::sin(th) * radius, y, std::cos(th) * radius};
            v.normal = {0, ny, 0};
            v.uv = {0.5f + 0.5f * std::sin(th), 0.5f + 0.5f * std::cos(th) * ny};
            m.vertices.push_back(v);
        }
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = centre + 1 + s, b = a + 1;
            if (cap == 0) m.indices.insert(m.indices.end(), {centre, a, b});
            else m.indices.insert(m.indices.end(), {centre, b, a});
        }
    }
    finish(m);
    return m;
}

}  // namespace primitives

// ------------------------------------------------------------------ library
MeshLibrary::MeshLibrary() {
    meshes_["cube"] = primitives::cube();
    meshes_["plane"] = primitives::plane();
    meshes_["sphere"] = primitives::sphere();
    meshes_["cylinder"] = primitives::cylinder();
    meshes_["capsule"] = primitives::capsule();
    materials_["default"] = Material{};
}

const MeshData* MeshLibrary::mesh(std::string_view name) const {
    auto it = meshes_.find(std::string(name));
    return it != meshes_.end() ? &it->second : nullptr;
}

const Material* MeshLibrary::material(std::string_view name) const {
    auto it = materials_.find(name.empty() ? std::string("default") : std::string(name));
    return it != materials_.end() ? &it->second : nullptr;
}

void MeshLibrary::setMesh(std::string name, MeshData mesh) {
    if (!mesh.bounds.valid()) mesh.computeBounds();
    meshes_[std::move(name)] = std::move(mesh);
    ++revision_;
}

void MeshLibrary::setMaterial(std::string name, Material material) {
    materials_[std::move(name)] = std::move(material);
    ++revision_;
}

std::vector<std::string> MeshLibrary::meshNames() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : meshes_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------------ serialization
void registerScene3D(serial::Registry& reg) {
    using namespace scene3d;
    auto& t = reg.add<Transform3D>("Transform3D", {MGE_FIELD(Transform3D, position, Vec3),
                                                   MGE_FIELD_HINT(Transform3D, rotation, Vec4, Rotation),
                                                   MGE_FIELD(Transform3D, scale, Vec3)});
    t.onAdd = [](ecs::World& w, ecs::Entity e) {
        if (!w.has<WorldTransform3D>(e)) w.add<WorldTransform3D>(e);
        w.get<Transform3D>(e)->dirty = 1;
    };
    reg.add<MeshRenderer>("MeshRenderer", {MGE_FIELD(MeshRenderer, mesh, Str32),
                                           MGE_FIELD(MeshRenderer, material, Str32),
                                           MGE_FIELD_HINT(MeshRenderer, tint, U32, Color),
                                           MGE_FIELD_HINT(MeshRenderer, castShadows, U32, Bool),
                                           MGE_FIELD_HINT(MeshRenderer, visible, U32, Bool)});
}

}  // namespace mge::render
