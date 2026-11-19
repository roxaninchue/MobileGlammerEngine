#include "test_main.h"
#include "mge/render/mesh.h"
#include <cmath>
using namespace mge;
using namespace mge::render;

namespace {
// Every triangle faces the way its vertex normals point (counter-clockwise front faces), every
// normal and tangent is unit length and perpendicular, and the mesh validates.
bool wellFormed(const MeshData& m) {
    if (!m.validate() || m.indices.empty()) return false;
    for (size_t i = 0; i < m.indices.size(); i += 3) {
        const Vertex3D &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        const vec3 n = cross(b.position - a.position, c.position - a.position);
        if (length(n) < 1e-9f) return false;   // degenerate
        if (dot(n, a.normal + b.normal + c.normal) <= 0) return false;
    }
    for (const Vertex3D& v : m.vertices) {
        if (std::fabs(length(v.normal) - 1) > 1e-3f || std::fabs(length(v.tangent.xyz()) - 1) > 1e-3f) return false;
        if (std::fabs(dot(v.normal, v.tangent.xyz())) > 1e-3f || std::fabs(std::fabs(v.tangent.w) - 1) > 1e-6f) return false;
    }
    return true;
}
float area(const MeshData& m) {
    float a = 0;
    for (size_t i = 0; i < m.indices.size(); i += 3) {
        const vec3 p = m.vertices[m.indices[i]].position, q = m.vertices[m.indices[i + 1]].position,
                   r = m.vertices[m.indices[i + 2]].position;
        a += 0.5f * length(cross(q - p, r - p));
    }
    return a;
}
}  // namespace

TEST(mesh_primitives_are_well_formed) {
    const MeshData cube = primitives::cube({2, 1, 4});
    CHECK(wellFormed(cube) && cube.vertices.size() == 24 && cube.triangleCount() == 12);
    CHECK(nearlyEqual(cube.bounds.min, {-1, -0.5f, -2}) && nearlyEqual(cube.bounds.max, {1, 0.5f, 2}));
    CHECK(std::fabs(area(cube) - 2 * (2 * 1 + 1 * 4 + 2 * 4)) < 1e-3f);

    const MeshData plane = primitives::plane({4, 2}, 3);
    CHECK(wellFormed(plane) && plane.vertices.size() == 16 && plane.triangleCount() == 18);
    CHECK(std::fabs(area(plane) - 8) < 1e-4f);

    const MeshData sphere = primitives::sphere(1, 32, 16);
    CHECK(wellFormed(sphere));
    CHECK(std::fabs(area(sphere) - 4 * kPi) < 0.05f * 4 * kPi);   // tessellated: a bit under
    for (const Vertex3D& v : sphere.vertices) CHECK(std::fabs(length(v.position) - 1) < 1e-4f);

    const MeshData cyl = primitives::cylinder(0.5f, 2, 24);
    CHECK(wellFormed(cyl));
    CHECK(nearlyEqual(cyl.bounds.min.y, -1) && nearlyEqual(cyl.bounds.max.y, 1));

    const MeshData cap = primitives::capsule(0.5f, 3, 24, 6);
    CHECK(wellFormed(cap));
    CHECK(nearlyEqual(cap.bounds.min.y, -1.5f) && nearlyEqual(cap.bounds.max.y, 1.5f));
    CHECK(nearlyEqual(cap.bounds.max.x, 0.5f, 1e-3f));
}

TEST(mesh_normals_tangents_validation) {
    // A quad in XY with uvs along +X / +Y: normal +Z, tangent +X, bitangent +Y (w = +1).
    MeshData m;
    for (vec2 p : {vec2{0, 0}, vec2{1, 0}, vec2{1, 1}, vec2{0, 1}}) {
        Vertex3D v;
        v.position = {p.x, p.y, 0};
        v.uv = p;
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    m.computeNormals();
    m.computeTangents();
    for (const Vertex3D& v : m.vertices) {
        CHECK(nearlyEqual(v.normal, {0, 0, 1}));
        CHECK(nearlyEqual(v.tangent.xyz(), {1, 0, 0}) && v.tangent.w == 1.0f);
    }
    // Mirrored uvs flip the handedness.
    for (Vertex3D& v : m.vertices) v.uv.y = 1 - v.uv.y;
    m.computeTangents();
    CHECK(m.vertices[0].tangent.w == -1.0f);
    CHECK(m.fits16BitIndices() && m.indices16().size() == 6);
    std::string err;
    m.indices.push_back(9);
    CHECK(!m.validate(&err) && !err.empty());
    m.indices = {0, 1, 7};
    CHECK(!m.validate(&err) && err == "index out of range");
}

TEST(mesh_library) {
    MeshLibrary lib;
    for (const char* n : {"cube", "plane", "sphere", "cylinder", "capsule"}) CHECK(lib.mesh(n) != nullptr);
    CHECK(lib.material("") && lib.material("") == lib.material("default"));
    CHECK(!lib.mesh("teapot"));
    const uint64_t rev = lib.revision();
    lib.setMesh("big", primitives::cube({10, 10, 10}));
    Material gold;
    gold.baseColor = {1, 0.8f, 0.3f, 1};
    gold.metallic = 1;
    lib.setMaterial("gold", gold);
    CHECK(lib.revision() == rev + 2);
    CHECK(lib.mesh("big")->bounds.valid() && lib.material("gold")->metallic == 1);
    CHECK(lib.meshNames().front() == "big" && lib.meshNames().size() == 6);
}
