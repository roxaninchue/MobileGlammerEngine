// Vulkan 1.1 bootstrap: instance, device, swapchain with pre-rotation, a tile-GPU friendly
// clear pass (LOAD_OP_CLEAR / STORE_OP_STORE, no depth), 2 frames in flight.
#include "mge/render/renderer.h"
#include "mge/log.h"
#include "mge/assert.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <cstring>
#include <vector>

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) { MGE_LOGE("%s failed: %d", #x, int(r_)); return false; } \
    } while (0)

namespace mge::render {
namespace {

constexpr uint32_t kFramesInFlight = 2;

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

        uint32_t image = 0;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainDirty_ = true; return false; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { MGE_LOGE("acquire failed: %d", int(r)); return false; }
        vkResetFences(device_, 1, &f.fence);

        vkResetCommandPool(device_, f.pool, 0);   // cheaper than per-buffer reset
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);

        VkClearValue clear{};
        std::memcpy(clear.color.float32, &info.clearColor.x, sizeof(float) * 4);
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_;
        rp.framebuffer = images_[image].framebuffer;
        rp.renderArea.extent = extent_;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        // M1: draw calls go here.
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

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
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
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = exts;
        VK_CHECK(vkCreateDevice(gpu_, &dci, nullptr, &device_));
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

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
        return true;
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
            if ((f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
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
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
            for (auto& f : frames_) {
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
};

}  // namespace

Renderer* createVulkanRenderer() {
    auto* r = new VulkanRenderer();
    if (!r->init()) { delete r; return nullptr; }
    return r;
}

Renderer* createGlesRenderer() { return nullptr; }   // M0 #8

Renderer* createBestRenderer() {
    if (Renderer* r = createVulkanRenderer()) return r;
    MGE_LOGW("Vulkan unavailable, falling back to GLES3");
    return createGlesRenderer();
}

}  // namespace mge::render
