#pragma once
// Backend-agnostic sprite batching (M1 #14).
//  - One 36-byte SpriteInstance per sprite; the quad corners come from gl_VertexIndex /
//    gl_VertexID, so there are no vertex or index buffers at all.
//  - Sprites are sorted by layer, then texture, then submission order (stable), and each run
//    of equal (layer, texture) becomes one instanced draw.
//  - Sprites whose world-space bounds miss the camera rect are culled before sorting
//    (Camera2D, Rect and screen scaling live in camera.h).
// Pure CPU code: it lives in mge_core so the host tests cover it; the GLES3 and Vulkan
// renderers only upload instances() and replay batches().
#include <cstdint>
#include <vector>
#include "mge/math.h"
#include "mge/render/camera.h"
#include "mge/transform2d.h"

namespace mge::render {

// Renderer-side texture id. 0 is the built-in 1x1 white texture every backend provides.
using TextureId = uint32_t;
constexpr TextureId kWhiteTexture = 0;

// GPU layout (instance-rate attributes, tightly packed, stride 36):
//  loc 0  vec4  axisX.xy, axisY.xy   (R32G32B32A32_SFLOAT)   quad edge vectors, size baked in
//  loc 1  vec2  origin               (R32G32_SFLOAT)         world position of the q = (0,0) corner (bottom-left before rotation)
//  loc 2  vec4  uv0.xy, uv1.xy       (R16G16B16A16_UNORM)    uv0 = top-left, uv1 = bottom-right
//  loc 3  vec4  color                (R8G8B8A8_UNORM)        R in the lowest byte
// Corner q in {0,1}^2: world = origin + axisX * q.x + axisY * q.y;
// u = mix(u0, u1, q.x), v = mix(v1, v0, q.y) so texel row 0 is the sprite's top edge.
struct SpriteInstance {
    float axisX[2];
    float axisY[2];
    float origin[2];
    uint16_t uv[4];
    uint32_t color;
};
static_assert(sizeof(SpriteInstance) == 36, "SpriteInstance must stay 36 bytes");

// RGBA8, R in the lowest byte (matches R8G8B8A8_UNORM and GL_UNSIGNED_BYTE on little-endian).
uint32_t packColor(vec4 rgba);
vec4 unpackColor(uint32_t packed);
inline uint32_t packGrey(float v, float a = 1.0f) { return packColor({v, v, v, a}); }

struct Sprite {
    scene2d::Affine2 world;          // entity world transform
    vec2 size{1, 1};                 // world units before the transform
    vec2 pivot{0.5f, 0.5f};          // 0..1 inside the quad; rotation/scale happen around it
    vec4 uvRect{0, 0, 1, 1};         // u0, v0 (top-left), u1, v1 (bottom-right)
    uint32_t color = 0xFFFFFFFFu;
    TextureId texture = kWhiteTexture;
    int16_t layer = 0;               // lower layers draw first
};

struct DrawBatch {
    TextureId texture;
    int16_t layer;
    uint32_t firstInstance;
    uint32_t instanceCount;
};

struct SpriteStats {
    uint32_t submitted = 0;
    uint32_t culled = 0;
    uint32_t drawn = 0;
    uint32_t batches = 0;
};

class SpriteBatcher {
public:
    void begin(const Camera2D& camera);
    void submit(const Sprite& s);
    // Already-built instances (e.g. a cached tilemap chunk), already culled by the caller.
    // Sorted as one unit with the sprites; `data` must stay alive until end().
    void submitInstances(int16_t layer, TextureId texture, const SpriteInstance* data, uint32_t count);
    void end();   // cull already happened in submit(); sorts and builds instances + batches

    const Camera2D& camera() const { return camera_; }
    const std::vector<SpriteInstance>& instances() const { return instances_; }
    const std::vector<DrawBatch>& batches() const { return batches_; }
    const SpriteStats& stats() const { return stats_; }

    bool cullingEnabled = true;

    // Exposed for tests and tools.
    static SpriteInstance makeInstance(const Sprite& s);
    static Rect worldBounds(const Sprite& s);

private:
    static constexpr uint32_t kPrebuilt = 0x80000000u;
    struct Key { int16_t layer; TextureId texture; uint32_t order; uint32_t ref; };   // ref: pending_ or prebuilt_ | kPrebuilt
    struct Prebuilt { const SpriteInstance* data; uint32_t count; };
    Camera2D camera_;
    Rect view_{};
    std::vector<Sprite> pending_;
    std::vector<Prebuilt> prebuilt_;
    std::vector<Key> keys_;
    std::vector<SpriteInstance> instances_;
    std::vector<DrawBatch> batches_;
    SpriteStats stats_;
};

}  // namespace mge::render
