#pragma once
// Backend-agnostic renderer interface. M0: clear screen only; grows into the render graph in M1.
#include <cstdint>
#include "mge/math.h"

struct ANativeWindow;

namespace mge::render {

enum class Backend { Vulkan, GLES3 };

struct FrameInfo {
    vec4 clearColor{0, 0, 0, 1};
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
};

Renderer* createVulkanRenderer();   // nullptr if Vulkan is unavailable or unsuitable
Renderer* createGlesRenderer();     // nullptr if no ES 3.0 context can be created
// Try Vulkan first, fall back to GLES3. debug.mge.renderer=gles|vulkan forces one.
Renderer* createBestRenderer();

}  // namespace mge::render
