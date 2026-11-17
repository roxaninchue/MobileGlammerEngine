#include "engine_host.h"
#include "mge/log.h"
#include <chrono>
#include <cmath>
#include "mge/jobs.h"

namespace mge {

EngineHost& engineHost() { static EngineHost host; return host; }

bool EngineHost::create(AAssetManager* assets, std::string dataPath) {
    if (thread_.joinable()) return true;   // activity recreated, engine survives
    assets_ = assets;
    dataPath_ = std::move(dataPath);
    quit_ = false;
    thread_ = std::thread(&EngineHost::renderLoop, this);
    MGE_LOGI("Engine created (data: %s)", dataPath_.c_str());
    return true;
}

void EngineHost::setWindow(ANativeWindow* window) {
    std::lock_guard lock(mutex_);
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    windowDirty_ = true;
    cv_.notify_one();
}

void EngineHost::resize(int width, int height) {
    std::lock_guard lock(mutex_);
    width_ = width; height_ = height;
    sizeDirty_ = true;
    cv_.notify_one();
}

void EngineHost::setPaused(bool paused) {
    std::lock_guard lock(mutex_);
    paused_ = paused;
    cv_.notify_one();
}

void EngineHost::destroy() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        cv_.notify_one();
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    if (window_) { ANativeWindow_release(window_); window_ = nullptr; }
    MGE_LOGI("Engine destroyed");
}

void EngineHost::renderLoop() {
    MGE_LOGI("Render thread started");
    jobs::init();
    renderer_ = render::createBestRenderer();
    if (!renderer_) MGE_LOGE("No usable renderer (Vulkan and GLES3 both unavailable)");

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    for (;;) {
        ANativeWindow* window = nullptr;
        bool windowChanged = false, sizeChanged = false;
        {
            std::unique_lock lock(mutex_);
            // Sleep (zero CPU) when paused or without a surface: lean by default.
            cv_.wait(lock, [&] { return quit_ || (!paused_ && window_) || (windowDirty_ && !window_); });
            if (quit_) break;
            windowChanged = windowDirty_;
            sizeChanged = sizeDirty_;
            windowDirty_ = sizeDirty_ = false;
            window = window_;
            if (window) ANativeWindow_acquire(window);   // hold our own ref while we use it
        }
        if (renderer_) {
            if (windowChanged && attached_) {
                renderer_->detachWindow();
                ANativeWindow_release(attached_);
                attached_ = nullptr;
            }
            if (windowChanged && window) {
                if (renderer_->attachWindow(window)) { attached_ = window; ANativeWindow_acquire(attached_); }
                else MGE_LOGE("attachWindow failed");
            }
            if (sizeChanged && attached_) renderer_->resize(uint32_t(width_), uint32_t(height_));
            if (attached_) {
                // M0 demo: slowly cycling monochrome clear proves the loop runs. Present paces us to vsync.
                float t = std::chrono::duration<float>(clock::now() - start).count();
                float g = 0.08f + 0.06f * (0.5f + 0.5f * std::sin(t * 1.5f));
                render::FrameInfo fi;
                fi.clearColor = {g, g, g, 1};
                if (!renderer_->renderFrame(fi)) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (window) ANativeWindow_release(window);
    }
    if (renderer_) {
        renderer_->waitIdle();
        if (attached_) { renderer_->detachWindow(); ANativeWindow_release(attached_); attached_ = nullptr; }
        delete renderer_;
        renderer_ = nullptr;
    }
    jobs::shutdown();
    MGE_LOGI("Render thread stopped");
}

}
