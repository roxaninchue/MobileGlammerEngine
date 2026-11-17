#include "engine_host.h"
#include "mge/log.h"
#include <chrono>

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
    windowDirty_ = true;
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
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    for (;;) {
        ANativeWindow* window = nullptr;
        {
            std::unique_lock lock(mutex_);
            // Sleep (zero CPU) when paused or without a surface: lean by default.
            cv_.wait(lock, [&] { return quit_ || (!paused_ && window_); });
            if (quit_) break;
            if (windowDirty_) {
                windowDirty_ = false;
                MGE_LOGI("Surface %dx%d", width_, height_);
                // M0 #7/#8: (re)create swapchain / GL surface here.
            }
            window = window_;
        }
        (void)window;
        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        (void)dt;
        // M0 #7: renderer->beginFrame(); clear; present (present blocks on vsync).
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // placeholder until present paces us
    }
    MGE_LOGI("Render thread stopped");
}

}
