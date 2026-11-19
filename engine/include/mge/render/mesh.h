#pragma once
// 3D mesh data, materials and the MeshRenderer component (M3 #26). CPU side only: the
// renderers upload MeshData / Material (M3 #27).
//  - Vertex3D: interleaved position, normal, uv, tangent (xyz + handedness in w), 48 bytes.
//  - MeshData: vertices + triangle-list indices (32-bit in memory, 16-bit for upload when the
//    vertex count allows), local bounds, normal and tangent generation, validation.
//  - primitives: cube, plane, sphere, capsule, cylinder with correct normals, uvs and tangents,
//    counter-clockwise front faces, Y up.
//  - Material: metallic-roughness PBR parameters (glTF 2.0 compatible).
//  - MeshLibrary: meshes and materials by name ("cube", "sphere"... are built in), so scenes
//    reference them by name and survive save/load.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "mge/math.h"

namespace mge::serial { class Registry; }

namespace mge::render {

struct Vertex3D {
    vec3 position;
    vec3 normal{0, 1, 0};
    vec2 uv;
    vec4 tangent{1, 0, 0, 1};   // xyz tangent, w = bitangent sign
};
static_assert(sizeof(Vertex3D) == 48, "Vertex3D is uploaded as-is");

struct MeshData {
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;   // triangle list, counter-clockwise front faces
    AABB bounds;

    uint32_t triangleCount() const { return uint32_t(indices.size() / 3); }
    void computeBounds();
    // Smooth, area-weighted vertex normals from the triangles.
    void computeNormals();
    // Per-vertex tangents from uvs (Lengyel's method), Gram-Schmidt against the normal.
    void computeTangents();
    // Fits 16-bit indices (the cheaper upload on mobile GPUs).
    bool fits16BitIndices() const { return vertices.size() <= 65536; }
    std::vector<uint16_t> indices16() const;
    // Index count a multiple of 3, every index in range, finite positions; why not in `error`.
    bool validate(std::string* error = nullptr) const;
};

namespace primitives {
MeshData cube(vec3 size = {1, 1, 1});                                 // 24 vertices, flat faces
MeshData plane(vec2 size = {1, 1}, uint32_t subdivisions = 1);       // XZ plane facing +Y
MeshData sphere(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16);
MeshData cylinder(float radius = 0.5f, float height = 1, uint32_t segments = 32);   // capped
MeshData capsule(float radius = 0.5f, float height = 2, uint32_t segments = 32, uint32_t rings = 8);  // height incl. caps
}  // namespace primitives

enum class AlphaMode : uint32_t { Opaque, Mask, Blend };

struct Material {
    vec4 baseColor{1, 1, 1, 1};
    vec3 emissive{0, 0, 0};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;
    std::string baseColorTexture;   // asset path, empty = white
    std::string normalTexture;
    std::string metallicRoughnessTexture;   // glTF packing: G = roughness, B = metallic
};

// Meshes and materials by name. The built-in meshes ("cube", "plane", "sphere", "cylinder",
// "capsule", all unit sized) and the "default" material always exist.
class MeshLibrary {
public:
    MeshLibrary();
    const MeshData* mesh(std::string_view name) const;
    const Material* material(std::string_view name) const;   // empty name = "default"
    // Adds or replaces; bumps revision() so renderers re-upload.
    void setMesh(std::string name, MeshData mesh);
    void setMaterial(std::string name, Material material);
    std::vector<std::string> meshNames() const;   // sorted
    uint64_t revision() const { return revision_; }

private:
    std::unordered_map<std::string, MeshData> meshes_;
    std::unordered_map<std::string, Material> materials_;
    uint64_t revision_ = 1;
};

// Draws a library mesh with a library material at the entity's WorldTransform3D.
struct MeshRenderer {
    char mesh[32] = "cube";      // MeshLibrary name
    char material[32] = "";      // MeshLibrary name, empty = "default"
    uint32_t tint = 0xFFFFFFFF;  // RGBA8 (R in the low byte), multiplies the base colour
    uint32_t castShadows = 1;
    uint32_t visible = 1;
};

// Transform3D (+ WorldTransform3D on load) and MeshRenderer.
void registerScene3D(serial::Registry& reg);

}  // namespace mge::render
