// Backend selection. Vulkan first, GLES3 fallback.
// Force a backend for testing: adb shell setprop debug.mge.renderer gles   (or vulkan)
#include "mge/render/renderer.h"
#include "mge/log.h"

#include <sys/system_properties.h>
#include <cstring>

namespace mge::render {

Renderer* createBestRenderer() {
    char forced[PROP_VALUE_MAX] = {};
    __system_property_get("debug.mge.renderer", forced);

    if (std::strcmp(forced, "gles") != 0) {
        if (Renderer* r = createVulkanRenderer()) { MGE_LOGI("Renderer: Vulkan"); return r; }
        if (std::strcmp(forced, "vulkan") == 0) { MGE_LOGE("Vulkan forced but unavailable"); return nullptr; }
        MGE_LOGW("Vulkan unavailable, falling back to GLES3");
    } else {
        MGE_LOGI("Renderer forced to GLES3 via debug.mge.renderer");
    }
    Renderer* r = createGlesRenderer();
    if (r) MGE_LOGI("Renderer: GLES3");
    return r;
}

}  // namespace mge::render
