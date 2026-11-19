#pragma once
// Backend-agnostic renderer interface. M1: clear + instanced sprite batches.
#include <cstdint>
#include "mge/math.h"
#include "mge/render/sprite_batch.h"
#include "mge/texture.h"

struct ANativeWindow;

namespace mge::render {

enum class Backend { Vulkan, GLES3 };

struct FrameInfo {
    vec4 clearColor{0, 0, 0, 1};               // fills the viewport
    vec4 letterboxColor{0, 0, 0, 1};           // fills the surface outside the viewport
    // Where the sprites' camera draws, in app-orientation surface pixels (ScreenLayout::viewport).
    // Full (the default) = the whole surface. Sprites are scissored to it.
    Viewport viewport{};
    const SpriteBatcher* sprites = nullptr;   // after end(); null = clear only
};

struct RenderStats {
    uint32_t drawCalls = 0;
    uint32_t sprites = 0;
    uint32_t textures = 0;          // live GPU textures, white included
    uint64_t textureBytes = 0;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual Backend backend() const = 0;
    // Window lifetime is owned by the host; the renderer only borrows it.
    virtual bool attachWindow(ANativeWindow* window) = 0;
    virtual void detachWindow() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    // Returns false if the frame was skipped (e.g. swapchain being rebuilt).
    virtual bool renderFrame(const FrameInfo& frame) = 0;
    virtual void waitIdle() = 0;
    // True when the last frame went out stale (the display rotated) and the next frame will be
    // laid out correctly: an on-demand loop should draw once more even if nothing changed.
    virtual bool wantsFrame() const { return false; }

    // Size in the app's orientation (pre-rotation undone), for camera aspect.
    virtual void viewportSize(uint32_t& width, uint32_t& height) const = 0;

    // Textures. Upload needs a device/context, i.e. after the first successful attachWindow.
    // Formats the GPU cannot sample are rejected (nothing is decoded on the CPU): createTexture
    // then returns kWhiteTexture. destroyTexture defers the free until no frame uses it.
    virtual bool supportsFormat(assets::TextureFormat f) const = 0;
    virtual TextureId createTexture(const assets::TextureData& data) = 0;
    virtual void destroyTexture(TextureId id) = 0;

    virtual RenderStats lastFrameStats() const = 0;
};

Renderer* createVulkanRenderer();   // nullptr if Vulkan is unavailable or unsuitable
Renderer* createGlesRenderer();     // nullptr if no ES 3.0 context can be created
// Try Vulkan first, fall back to GLES3. debug.mge.renderer=gles|vulkan forces one.
Renderer* createBestRenderer();

}  // namespace mge::render
