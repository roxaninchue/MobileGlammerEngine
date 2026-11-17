#pragma once
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include "mge/render/renderer.h"

namespace mge {

// Owns the native render thread and the window. Java only posts events; all
// engine work happens on the render thread so the UI thread never blocks on a frame.
class EngineHost {
public:
    bool create(AAssetManager* assets, std::string dataPath);
    void setWindow(ANativeWindow* window);   // takes ownership of one reference; nullptr = surface gone
    void resize(int width, int height);
    void setPaused(bool paused);
    void destroy();

private:
    void renderLoop();

    AAssetManager* assets_ = nullptr;
    std::string dataPath_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    ANativeWindow* window_ = nullptr;
    int width_ = 0, height_ = 0;
    bool paused_ = false;
    bool quit_ = false;
    bool windowDirty_ = false;
    bool sizeDirty_ = false;
    render::Renderer* renderer_ = nullptr;
    ANativeWindow* attached_ = nullptr;
};

EngineHost& engineHost();
}
