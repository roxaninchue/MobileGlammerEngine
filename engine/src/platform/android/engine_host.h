#pragma once
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
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
    void touch(const input::TouchEvent& e);
    // Editor panel channel (any thread): one protocol line (see mge/editor/editor_session.h),
    // applied on the render thread before the next frame.
    void editorCommand(std::string line);
    // Latest panel state JSON when its revision differs from `sinceRevision`; false otherwise.
    bool editorState(uint64_t sinceRevision, std::string& out);
    // Screen density (px per dp) for gesture thresholds.
    void setDensity(float density);
    // Latest on-screen stats (a few short lines), refreshed twice a second. Any thread.
    std::string statsText();

private:
    struct FrameState {
        ANativeWindow* window = nullptr;
        render::Insets insets{};
        bool windowChanged = false, sizeChanged = false, densityChanged = false;
        bool paused = false;     // app in the background: no frame, only housekeeping
        bool autosave = false;   // editor: save the open project now (the app was just paused)
        float density = 1.0f;
    };
    void renderLoop();
    void editorLoop(const char* backendName);
    void demoLoop(const char* backendName);
    // Blocks until a frame is wanted (always when `wantFrame`, else on input / commands /
    // surface changes), then applies window and size changes. False when quitting.
    bool nextFrame(bool wantFrame, FrameState& fs);
    void endFrame(FrameState& fs);
    void wakeUp();

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
    bool editorRunning_ = false;   // editorLoop owns the session (autosave on pause)
    bool autosaveWanted_ = false;
    bool quit_ = false;
    bool windowDirty_ = false;
    bool sizeDirty_ = false;
    bool wake_ = false;
    std::mutex editorMutex_;
    std::vector<std::string> editorCommands_;
    std::string editorStateJson_;
    uint64_t editorRevision_ = 0;
    std::mutex statsMutex_;
    std::string statsText_;
    render::Renderer* renderer_ = nullptr;
    ANativeWindow* attached_ = nullptr;
};

EngineHost& engineHost();
}
