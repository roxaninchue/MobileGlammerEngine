#pragma once
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include "mge/input.h"
#include "mge/render/renderer.h"

namespace mge {

// Owns the native render thread and the window. Java only posts events; all
// engine work happens on the render thread so the UI thread never blocks on a frame.
class EngineHost {
public:
    bool create(AAssetManager* assets, std::string dataPath);
    void setWindow(ANativeWindow* window);   // takes ownership of one reference; nullptr = surface gone
    void resize(int width, int height);
    // Safe-area insets in surface pixels (system bars, display cutout), from WindowInsets.
    void setInsets(int left, int top, int right, int bottom);
    void setPaused(bool paused);
    void destroy();

    // UI thread: raw touch events (surface pixels, CLOCK_MONOTONIC ns). Never blocks on a frame.
    void touch(const input::TouchEvent& e) { touches_.push(e); }
    // Screen density (px per dp) for gesture thresholds.
    void setDensity(float density);
    // Latest on-screen stats (a few short lines), refreshed twice a second. Any thread.
    std::string statsText();

private:
    void renderLoop();

    AAssetManager* assets_ = nullptr;
    std::string dataPath_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    ANativeWindow* window_ = nullptr;
    int width_ = 0, height_ = 0;
    render::Insets insets_{};
    input::TouchQueue touches_{256};
    float density_ = 1.0f;
    bool densityDirty_ = false;
    bool paused_ = false;
    bool quit_ = false;
    bool windowDirty_ = false;
    bool sizeDirty_ = false;
    std::mutex statsMutex_;
    std::string statsText_;
    render::Renderer* renderer_ = nullptr;
    ANativeWindow* attached_ = nullptr;
};

EngineHost& engineHost();
}
