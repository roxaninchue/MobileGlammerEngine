// Vulkan 1.1 renderer: instance, device, swapchain with pre-rotation, a tile-GPU friendly
// pass (LOAD_OP_CLEAR / STORE_OP_STORE, no depth), 2 frames in flight.
// Sprites (M1 #14):
//  - One pipeline, no vertex buffers: corners from gl_VertexIndex, a single instance-rate
//    binding pointing at this frame's persistently mapped instance buffer, vkCmdDraw with
//    firstInstance per batch. Pre-rotation is a 2x2 in the push constants.
//  - Textures (RGBA8, and ETC2/ASTC when the device samples them) are staged at create time
//    and the copies are recorded into the next frame's own command buffer, ahead of the
//    render pass. Staging buffers and destroyed textures are freed only once every frame
//    that could still reference them has signalled its fence.
//  - The swapchain is UNORM (not sRGB) so the same colours come out as on GLES, which
//    writes to a linear default framebuffer.
#include "mge/render/renderer.h"
#include "mge/log.h"
#include "mge/assert.h"
#include "generated/sprite_spv.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) { MGE_LOGE("%s failed: %d", #x, int(r_)); return false; } \
    } while (0)

namespace mge::render {
namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxTextures = 1024;   // descriptor pool size; one set per texture

using assets::TextureFormat;

VkFormat vkFormat(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::ETC2_RGB8: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        case TextureFormat::ETC2_RGBA8: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        case TextureFormat::ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case TextureFormat::ASTC_6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case TextureFormat::ASTC_8x8: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

struct PushConstants {
    float view[4];        // world->ndc scale.xy, offset.zw
    float preRotate[4];   // column-major 2x2
};

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override { shutdown(); }
    Backend backend() const override { return Backend::Vulkan; }

    bool init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Glammer";
        app.pEngineName = "MobileGlammerEngine";
        app.apiVersion = VK_API_VERSION_1_1;

        uint32_t apiVersion = VK_API_VERSION_1_0;
        auto enumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        if (enumVer) enumVer(&apiVersion);
        if (apiVersion < VK_API_VERSION_1_1) { MGE_LOGW("Vulkan 1.1 not available"); return false; }

        const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;
#ifndef NDEBUG
        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        if (hasLayer(layers[0])) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
#endif
        VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));
        return pickDevice();
    }

    bool attachWindow(ANativeWindow* window) override {
        window_ = window;
        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = window;
        VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_));
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu_, queueFamily_, surface_, &supported);
        if (!supported) { MGE_LOGE("Queue cannot present to surface"); return false; }
        if (!device_ && !createDevice()) return false;
        return createSwapchain();
    }

    void detachWindow() override {
        if (device_) vkDeviceWaitIdle(device_);
        destroySwapchain();
        if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
        window_ = nullptr;
    }

    void resize(uint32_t, uint32_t) override { swapchainDirty_ = true; }

    bool renderFrame(const FrameInfo& info) override {
        if (!swapchain_) return false;
        if (swapchainDirty_) { if (!recreateSwapchain()) return false; }

        Frame& f = frames_[frameIndex_];
        vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        if (f.number > completedFrame_) completedFrame_ = f.number;
        collectGarbage();

        uint32_t image = 0;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainDirty_ = true; return false; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { MGE_LOGE("acquire failed: %d", int(r)); return false; }
        vkResetFences(device_, 1, &f.fence);

        // Fill this frame's instance buffer (the fence above proves the GPU is done with it).
        const SpriteBatcher* sprites = info.sprites;
        if (sprites && !sprites->instances().empty() && !writeInstances(f, *sprites)) sprites = nullptr;

        vkResetCommandPool(device_, f.pool, 0);   // cheaper than per-buffer reset
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        recordUploads(f.cmd);
        stats_ = {};

        VkClearValue clear{};
        const vec4& passClear = info.viewport.full() ? info.clearColor : info.letterboxColor;
        std::memcpy(clear.color.float32, &passClear.x, sizeof(float) * 4);
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_;
        rp.framebuffer = images_[image].framebuffer;
        rp.renderArea.extent = extent_;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        // Letterboxing: the pass cleared the whole image to the bar colour; clear the camera's
        // viewport to the scene colour and scissor the sprites to it.
        uint32_t appW = 0, appH = 0;
        viewportSize(appW, appH);
        const Viewport vp = clampViewport(info.viewport, appW, appH);
        VkRect2D scissor{{0, 0}, extent_};
        if (!vp.full()) {
            const Viewport n = rotateToNative(vp, appW, appH, quarterTurns());
            scissor = {{n.x, n.y}, {n.w, n.h}};
            VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
            std::memcpy(ca.clearValue.color.float32, &info.clearColor.x, sizeof(float) * 4);
            VkClearRect cr{scissor, 0, 1};
            vkCmdClearAttachments(f.cmd, 1, &ca, 1, &cr);
        }
        if (sprites) drawSprites(f, *sprites, vp, appW, appH, scissor);
        vkCmdEndRenderPass(f.cmd);
        vkEndCommandBuffer(f.cmd);

        // Per-image "rendered" semaphore avoids reusing a semaphore still held by the presentation engine.
        VkSemaphore rendered = images_[image].rendered;
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered;
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, f.fence));
        f.number = ++submittedFrame_;
        countTextures();

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &image;
        r = vkQueuePresentKHR(queue_, &pi);
        // SUBOPTIMAL on Android usually means rotation changed; we pre-rotate so only rebuild on OUT_OF_DATE.
        if (r == VK_ERROR_OUT_OF_DATE_KHR) swapchainDirty_ = true;

        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
        return true;
    }

    void waitIdle() override { if (device_) vkDeviceWaitIdle(device_); }

    void viewportSize(uint32_t& w, uint32_t& h) const override {
        bool swapped = transform_ & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
        w = swapped ? extent_.height : extent_.width;
        h = swapped ? extent_.width : extent_.height;
    }

    RenderStats lastFrameStats() const override { return stats_; }

    bool supportsFormat(TextureFormat f) const override {
        if (!gpu_) return false;
        if ((f == TextureFormat::ETC2_RGB8 || f == TextureFormat::ETC2_RGBA8) && !features_.textureCompressionETC2) return false;
        if ((f == TextureFormat::ASTC_4x4 || f == TextureFormat::ASTC_6x6 || f == TextureFormat::ASTC_8x8) &&
            !features_.textureCompressionASTC_LDR) return false;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, vkFormat(f), &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    }

    TextureId createTexture(const assets::TextureData& data) override {
        if (!device_ || data.mipCount == 0 || data.bytes.empty()) return kWhiteTexture;
        if (!supportsFormat(data.format)) {
            MGE_LOGW("Vulkan: %s not supported by this GPU, using white", assets::formatName(data.format));
            return kWhiteTexture;
        }
        uint32_t id;
        if (!freeIds_.empty()) { id = freeIds_.back(); freeIds_.pop_back(); }
        else { id = uint32_t(textures_.size()); textures_.emplace_back(); }
        if (!createTextureAt(id, data)) {
            textures_[id] = Tex{};
            freeIds_.push_back(id);
            return kWhiteTexture;
        }
        return id;
    }

    void destroyTexture(TextureId id) override {
        if (id == kWhiteTexture || id >= textures_.size() || !textures_[id].live) return;
        Tex& t = textures_[id];
        // A queued upload that never ran: drop it (its staging buffer goes to the garbage too).
        for (size_t i = 0; i < uploads_.size(); ++i) {
            if (uploads_[i].texture != id) continue;
            retire(uploads_[i].staging, uploads_[i].stagingMemory);
            uploads_.erase(uploads_.begin() + std::ptrdiff_t(i));
            break;
        }
        Garbage g;
        g.frame = submittedFrame_;   // the last submitted frame may still sample it
        g.image = t.image; g.view = t.view; g.memory = t.memory; g.set = t.set;
        garbage_.push_back(g);
        t = Tex{};
        freeIds_.push_back(id);
    }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t number = 0;                       // submittedFrame_ value of its last submit
        VkBuffer instances = VK_NULL_HANDLE;       // host-visible, coherent, persistently mapped
        VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
        void* instanceMap = nullptr;
        VkDeviceSize instanceCapacity = 0;
    };
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t bytes = 0;
        bool live = false;
        bool uploaded = false;
    };
    struct Upload {
        TextureId texture;
        VkBuffer staging;
        VkDeviceMemory stagingMemory;
        uint32_t mipCount;
        uint32_t width[assets::kMaxMips], height[assets::kMaxMips];
        VkDeviceSize offset[assets::kMaxMips];
    };
    // Freed once completedFrame_ >= frame, i.e. no in-flight command buffer can reference it.
    struct Garbage {
        uint64_t frame = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct SwapImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
    };

    static bool hasLayer(const char* name) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props) if (std::strcmp(p.layerName, name) == 0) return true;
        return false;
    }

    bool pickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (n == 0) { MGE_LOGW("No Vulkan devices"); return false; }
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
        for (auto g : gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(g, &p);
            if (p.apiVersion < VK_API_VERSION_1_1) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
            for (uint32_t i = 0; i < qn; ++i) {
                if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    gpu_ = g;
                    queueFamily_ = i;
                    MGE_LOGI("Vulkan GPU: %s (API %u.%u, driver 0x%x)", p.deviceName,
                             VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), p.driverVersion);
                    return true;
                }
            }
        }
        MGE_LOGW("No Vulkan 1.1 device with a graphics queue");
        return false;
    }

    bool createDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        // Enable only the compressed-texture features the device has; supportsFormat() reads them.
        VkPhysicalDeviceFeatures avail{};
        vkGetPhysicalDeviceFeatures(gpu_, &avail);
        features_ = {};
        features_.textureCompressionETC2 = avail.textureCompressionETC2;
        features_.textureCompressionASTC_LDR = avail.textureCompressionASTC_LDR;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = exts;
        dci.pEnabledFeatures = &features_;
        VK_CHECK(vkCreateDevice(gpu_, &dci, nullptr, &device_));
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        vkGetPhysicalDeviceMemoryProperties(gpu_, &memProps_);
        MGE_LOGI("Vulkan: ETC2 %s, ASTC %s", features_.textureCompressionETC2 ? "yes" : "no",
                 features_.textureCompressionASTC_LDR ? "yes" : "no");

        for (auto& f : frames_) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = queueFamily_;
            VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd));
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.acquired));
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.fence));
        }
        if (!createSpriteResources()) return false;

        // Texture 0: 1x1 white, so untextured sprites share the same pipeline and set layout.
        assets::TextureData white;
        white.format = TextureFormat::RGBA8;
        white.width = white.height = white.mipCount = 1;
        white.mips[0] = {1, 1, 0, 4};
        white.bytes = {255, 255, 255, 255};
        textures_.emplace_back();
        return createTextureAt(kWhiteTexture, white);
    }

    bool createSwapchain() {
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps));

        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, formats.data());
        VkSurfaceFormatKHR fmt = formats[0];
        for (auto& f : formats)
            if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; break; }
        format_ = fmt.format;

        // Pre-rotation: render in the display's native orientation so the compositor
        // doesn't spend a GPU pass rotating our frame (a real battery and latency win).
        extent_ = caps.currentExtent;
        transform_ = caps.currentTransform;
        if (transform_ & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
            std::swap(extent_.width, extent_.height);
        if (extent_.width == 0 || extent_.height == 0) return false;   // minimized

        uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;   // triple buffer for steady pacing
        if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

        VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                                                ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                                : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface_;
        sci.minImageCount = count;
        sci.imageFormat = fmt.format;
        sci.imageColorSpace = fmt.colorSpace;
        sci.imageExtent = extent_;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = transform_;
        sci.compositeAlpha = alpha;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync; always supported, lowest power
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain_;
        VkSwapchainKHR newSwap = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &newSwap));
        destroySwapchain();
        swapchain_ = newSwap;

        if (!renderPass_ && !createRenderPass()) return false;
        if (!pipeline_ && !createPipeline()) return false;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        std::vector<VkImage> imgs(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, imgs.data());
        images_.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            SwapImage& s = images_[i];
            s.image = imgs[i];
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = s.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = format_;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &s.view));
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = renderPass_;
            fci.attachmentCount = 1;
            fci.pAttachments = &s.view;
            fci.width = extent_.width;
            fci.height = extent_.height;
            fci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &s.framebuffer));
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device_, &semi, nullptr, &s.rendered));
        }
        swapchainDirty_ = false;
        MGE_LOGI("Swapchain %ux%u, %u images, transform 0x%x", extent_.width, extent_.height, n, unsigned(transform_));
        return true;
    }

    bool createRenderPass() {
        // Tile-GPU friendly: clear on load (no read from memory), store only color, nothing else.
        VkAttachmentDescription color{};
        color.format = format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount = 1;
        rci.pAttachments = &color;
        rci.subpassCount = 1;
        rci.pSubpasses = &sub;
        rci.dependencyCount = 1;
        rci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &rci, nullptr, &renderPass_));
        return true;
    }

    // ------------------------------------------------------------ sprites: resources
    uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
        return UINT32_MAX;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** map) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        uint32_t type = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) { MGE_LOGE("No host-visible coherent memory"); vkDestroyBuffer(device_, buf, nullptr); buf = VK_NULL_HANDLE; return false; }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) {
            MGE_LOGE("buffer allocation (%llu bytes) failed", static_cast<unsigned long long>(size));
            vkDestroyBuffer(device_, buf, nullptr);
            buf = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device_, buf, mem, 0);
        if (map) VK_CHECK(vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, map));
        return true;
    }

    bool createSpriteResources() {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1;
        lci.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &setLayout_));

        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_));

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = kMaxTextures;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        VK_CHECK(vkCreateDescriptorPool(device_, &dpi, nullptr, &descriptorPool_));

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_));
        return true;
    }

    VkShaderModule shaderModule(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) MGE_LOGE("vkCreateShaderModule failed");
        return m;
    }

    bool createPipeline() {
        VkShaderModule vs = shaderModule(spv::sprite_vert, sizeof(spv::sprite_vert));
        VkShaderModule fs = shaderModule(spv::sprite_frag, sizeof(spv::sprite_frag));
        if (!vs || !fs) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            if (fs) vkDestroyShaderModule(device_, fs, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        // One instance-rate binding; the 4 strip corners come from gl_VertexIndex.
        VkVertexInputBindingDescription bind{0, sizeof(SpriteInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription attrs[4] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(SpriteInstance, axisX))},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(SpriteInstance, origin))},
            {2, 0, VK_FORMAT_R16G16B16A16_UNORM, uint32_t(offsetof(SpriteInstance, uv))},
            {3, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(SpriteInstance, color))},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;   // mirrored (negative scale) sprites must still draw
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;   // same as GLES glBlendFunc
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &ds;
        gp.layout = pipelineLayout_;
        gp.renderPass = renderPass_;
        gp.subpass = 0;
        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (r != VK_SUCCESS) { MGE_LOGE("sprite pipeline creation failed: %d", int(r)); return false; }
        return true;
    }

    // ------------------------------------------------------------ sprites: textures
    // Creates the image + view + set now and queues the copy for the next frame's command buffer.
    bool createTextureAt(uint32_t id, const assets::TextureData& d) {
        Tex t;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = vkFormat(d.format);
        ici.extent = {d.width, d.height, 1};
        ici.mipLevels = d.mipCount;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS) { MGE_LOGE("vkCreateImage failed"); return false; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, t.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &t.memory) != VK_SUCCESS) {
            MGE_LOGE("texture memory allocation (%llu bytes) failed", static_cast<unsigned long long>(req.size));
            vkDestroyImage(device_, t.image, nullptr);
            return false;
        }
        vkBindImageMemory(device_, t.image, t.memory, 0);
        t.bytes = req.size;

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, d.mipCount, 0, 1};
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descriptorPool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout_;
        if (vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS ||
            vkAllocateDescriptorSets(device_, &dai, &t.set) != VK_SUCCESS) {
            MGE_LOGE("texture view/descriptor failed (pool holds %u textures)", kMaxTextures);
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
            return false;
        }
        VkDescriptorImageInfo dii{sampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = t.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

        // Staging: each mip at a 16-byte aligned offset (covers every block size we load).
        Upload up{};
        up.texture = id;
        up.mipCount = d.mipCount;
        VkDeviceSize total = 0;
        for (uint32_t m = 0; m < d.mipCount; ++m) {
            up.offset[m] = total;
            up.width[m] = d.mips[m].width;
            up.height[m] = d.mips[m].height;
            total += (VkDeviceSize(d.mips[m].size) + 15) & ~VkDeviceSize(15);
        }
        void* map = nullptr;
        if (!createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, up.staging, up.stagingMemory, &map)) {
            vkFreeDescriptorSets(device_, descriptorPool_, 1, &t.set);
            vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
            return false;
        }
        for (uint32_t m = 0; m < d.mipCount; ++m)
            std::memcpy(static_cast<uint8_t*>(map) + up.offset[m], d.bytes.data() + d.mips[m].offset, d.mips[m].size);
        vkUnmapMemory(device_, up.stagingMemory);

        t.live = true;
        textures_[id] = t;
        uploads_.push_back(up);
        return true;
    }

    void recordUploads(VkCommandBuffer cmd) {
        if (uploads_.empty()) return;
        for (const Upload& up : uploads_) {
            Tex& t = textures_[up.texture];
            VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toDst.srcAccessMask = 0;
            toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image = t.image;
            toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, up.mipCount, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &toDst);
            VkBufferImageCopy regions[assets::kMaxMips] = {};
            for (uint32_t m = 0; m < up.mipCount; ++m) {
                regions[m].bufferOffset = up.offset[m];
                regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                regions[m].imageExtent = {up.width[m], up.height[m], 1};
            }
            vkCmdCopyBufferToImage(cmd, up.staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, up.mipCount, regions);
            VkImageMemoryBarrier toRead = toDst;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toRead);
            t.uploaded = true;
            // The staging buffer lives until this frame (the next one submitted) has finished.
            Garbage g;
            g.frame = submittedFrame_ + 1;
            g.buffer = up.staging;
            g.memory = up.stagingMemory;
            garbage_.push_back(g);
        }
        uploads_.clear();
    }

    void retire(VkBuffer buffer, VkDeviceMemory memory) {
        Garbage g;
        g.frame = submittedFrame_;
        g.buffer = buffer;
        g.memory = memory;
        garbage_.push_back(g);
    }

    void freeGarbage(const Garbage& g) {
        if (g.set) vkFreeDescriptorSets(device_, descriptorPool_, 1, &g.set);
        if (g.view) vkDestroyImageView(device_, g.view, nullptr);
        if (g.image) vkDestroyImage(device_, g.image, nullptr);
        if (g.buffer) vkDestroyBuffer(device_, g.buffer, nullptr);
        if (g.memory) vkFreeMemory(device_, g.memory, nullptr);
    }

    void collectGarbage() {
        size_t keep = 0;
        for (size_t i = 0; i < garbage_.size(); ++i) {
            if (garbage_[i].frame <= completedFrame_) freeGarbage(garbage_[i]);
            else garbage_[keep++] = garbage_[i];
        }
        garbage_.resize(keep);
    }

    // ------------------------------------------------------------ sprites: per frame
    bool writeInstances(Frame& f, const SpriteBatcher& batcher) {
        const auto& inst = batcher.instances();
        VkDeviceSize bytes = VkDeviceSize(inst.size()) * sizeof(SpriteInstance);
        if (bytes > f.instanceCapacity) {
            // This frame's fence has signalled, so its old buffer is idle and can go right away.
            if (f.instances) {
                vkDestroyBuffer(device_, f.instances, nullptr);
                vkFreeMemory(device_, f.instanceMemory, nullptr);
                f.instances = VK_NULL_HANDLE;
                f.instanceMemory = VK_NULL_HANDLE;
                f.instanceMap = nullptr;
                f.instanceCapacity = 0;
            }
            VkDeviceSize cap = bytes + bytes / 2;
            if (cap < 64 * 1024) cap = 64 * 1024;
            if (!createBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, f.instances, f.instanceMemory, &f.instanceMap))
                return false;
            f.instanceCapacity = cap;
        }
        std::memcpy(f.instanceMap, inst.data(), size_t(bytes));   // coherent: no flush needed
        return true;
    }

    void preRotation(float out[4]) const {
        // Column-major 2x2 rotating clip space by the surface transform (CCW angle).
        float c = 1, s = 0;
        if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { c = 0; s = 1; }
        else if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) { c = -1; s = 0; }
        else if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) { c = 0; s = -1; }
        out[0] = c; out[1] = s; out[2] = -s; out[3] = c;
    }

    int quarterTurns() const {
        if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) return 1;
        if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) return 2;
        if (transform_ & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) return 3;
        return 0;
    }

    // A viewport computed for a stale surface size is clipped to the current one.
    static Viewport clampViewport(Viewport vp, uint32_t appW, uint32_t appH) {
        if (vp.full()) return vp;
        const int32_t W = int32_t(appW), H = int32_t(appH);
        int32_t x0 = vp.x < 0 ? 0 : vp.x, y0 = vp.y < 0 ? 0 : vp.y;
        int32_t x1 = vp.x + int32_t(vp.w), y1 = vp.y + int32_t(vp.h);
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        if (x1 <= x0 || y1 <= y0) return Viewport{};
        if (x0 == 0 && y0 == 0 && x1 == W && y1 == H) return Viewport{};
        return {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};
    }

    void drawSprites(Frame& f, const SpriteBatcher& batcher, const Viewport& vp, uint32_t appW, uint32_t appH,
                     const VkRect2D& scissor) {
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        // The GPU viewport stays the whole image (pre-rotation works in full clip space); the
        // camera's viewport is folded into the view transform and enforced by the scissor.
        VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
        vkCmdSetViewport(f.cmd, 0, 1, &viewport);
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);

        const vec4 view = viewTransform(batcher.camera(), vp, appW, appH);
        PushConstants pc{{view.x, view.y, view.z, view.w}, {}};
        preRotation(pc.preRotate);
        vkCmdPushConstants(f.cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.instances, &zero);

        TextureId bound = UINT32_MAX;
        for (const DrawBatch& b : batcher.batches()) {
            TextureId id = b.texture < textures_.size() && textures_[b.texture].live && textures_[b.texture].uploaded
                               ? b.texture : kWhiteTexture;
            if (id != bound) {
                vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                        &textures_[id].set, 0, nullptr);
                bound = id;
            }
            vkCmdDraw(f.cmd, 4, b.instanceCount, 0, b.firstInstance);
            ++stats_.drawCalls;
            stats_.sprites += b.instanceCount;
        }
    }

    void countTextures() {
        stats_.textures = 0;
        stats_.textureBytes = 0;
        for (const Tex& t : textures_)
            if (t.live) { ++stats_.textures; stats_.textureBytes += t.bytes; }
    }

    bool recreateSwapchain() {
        vkDeviceWaitIdle(device_);
        destroySwapchainResources();
        return createSwapchain();
    }

    void destroySwapchainResources() {
        for (auto& s : images_) {
            if (s.framebuffer) vkDestroyFramebuffer(device_, s.framebuffer, nullptr);
            if (s.view) vkDestroyImageView(device_, s.view, nullptr);
            if (s.rendered) vkDestroySemaphore(device_, s.rendered, nullptr);
        }
        images_.clear();
    }

    void destroySwapchain() {
        if (!device_) return;
        destroySwapchainResources();
        if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    }

    void shutdown() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            destroySwapchain();
            for (const Garbage& g : garbage_) freeGarbage(g);
            garbage_.clear();
            for (const Upload& up : uploads_) { vkDestroyBuffer(device_, up.staging, nullptr); vkFreeMemory(device_, up.stagingMemory, nullptr); }
            uploads_.clear();
            for (const Tex& t : textures_) {
                if (!t.live) continue;
                vkDestroyImageView(device_, t.view, nullptr);
                vkDestroyImage(device_, t.image, nullptr);
                vkFreeMemory(device_, t.memory, nullptr);
            }
            textures_.clear();   // sets go with the pool
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
            for (auto& f : frames_) {
                if (f.instances) vkDestroyBuffer(device_, f.instances, nullptr);
                if (f.instanceMemory) vkFreeMemory(device_, f.instanceMemory, nullptr);
                if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
                if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
                if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<SwapImage> images_;
    Frame frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    bool swapchainDirty_ = false;
    ANativeWindow* window_ = nullptr;

    VkPhysicalDeviceFeatures features_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<Tex> textures_;
    std::vector<uint32_t> freeIds_;
    std::vector<Upload> uploads_;
    std::vector<Garbage> garbage_;
    uint64_t submittedFrame_ = 0;   // frames submitted so far
    uint64_t completedFrame_ = 0;   // highest frame number whose fence we have seen signalled
    RenderStats stats_;
};

}  // namespace

Renderer* createVulkanRenderer() {
    auto* r = new VulkanRenderer();
    if (!r->init()) { delete r; return nullptr; }
    return r;
}

}  // namespace mge::render
