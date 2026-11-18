#include "engine_host.h"
#include "mge/log.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <sys/system_properties.h>
#include "mge/jobs.h"
#include "mge/render/camera.h"
#include "mge/render/sprite_batch.h"
#include "mge/render/tilemap.h"
#include "mge/texture.h"
#include "mge/transform2d.h"

namespace mge {
namespace {

// TextureCache reader over the APK's assets/. AAssetManager is safe to use from job workers.
bool assetReader(const char* path, std::vector<uint8_t>& out, void* user) {
    AAsset* a = AAssetManager_open(static_cast<AAssetManager*>(user), path, AASSET_MODE_BUFFER);
    if (!a) return false;
    off_t n = AAsset_getLength(a);
    bool ok = n > 0;
    if (ok) {
        out.resize(size_t(n));
        ok = AAsset_read(a, out.data(), out.size()) == int(out.size());
    }
    AAsset_close(a);
    return ok;
}

// M1 demo: a 64x36 grid of spinning monochrome sprites sharing the checker texture.
constexpr int kGridW = 64, kGridH = 36;
constexpr const char* kCheckerPath = "textures/checker.ktx";

// debug.mge.scale = letterbox|expand|crop|fitwidth|fitheight (default expand),
// debug.mge.pixelperfect = 1 to snap to whole screen pixels per texel.
render::DesignSpec demoDesign() {
    render::DesignSpec d;
    d.area = {{-kGridW * 0.5f, -kGridH * 0.5f - 1.0f}, {kGridW * 0.5f, kGridH * 0.5f + 1.0f}};
    char mode[PROP_VALUE_MAX] = {};
    __system_property_get("debug.mge.scale", mode);
    if (!std::strcmp(mode, "letterbox")) d.mode = render::ScaleMode::Letterbox;
    else if (!std::strcmp(mode, "crop")) d.mode = render::ScaleMode::Crop;
    else if (!std::strcmp(mode, "fitwidth")) d.mode = render::ScaleMode::FitWidth;
    else if (!std::strcmp(mode, "fitheight")) d.mode = render::ScaleMode::FitHeight;
    else d.mode = render::ScaleMode::Expand;
    char pp[PROP_VALUE_MAX] = {};
    __system_property_get("debug.mge.pixelperfect", pp);
    d.pixelPerfect = pp[0] == '1';
    d.artPixelsPerUnit = 16.0f;   // 16 texels per world unit when pixel perfect
    return d;
}

struct DemoStats {
    uint32_t frames = 0;
    double batchMs = 0, frameMs = 0;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

}  // namespace

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

void EngineHost::setInsets(int left, int top, int right, int bottom) {
    std::lock_guard lock(mutex_);
    auto clampPx = [](int v) { return uint32_t(v > 0 ? v : 0); };
    insets_ = {clampPx(left), clampPx(top), clampPx(right), clampPx(bottom)};
    cv_.notify_one();
}

void EngineHost::setDensity(float density) {
    std::lock_guard lock(mutex_);
    density_ = density > 0.0f ? density : 1.0f;
    densityDirty_ = true;
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

    assets::TextureCacheConfig cacheCfg;
    cacheCfg.reader = assetReader;
    cacheCfg.user = assets_;
    assets::TextureCache textures(cacheCfg);
    assets::TextureHandle checker = textures.acquire(kCheckerPath);
    render::TextureId checkerGpu = render::kWhiteTexture;
    bool checkerResolved = false;
    render::SpriteBatcher batcher;
    const render::DesignSpec design = demoDesign();
    render::ScreenLayout layout;
    // Floor behind the sprite grid: a 128x64 tile map (8x4 chunks), wider than any screen shows,
    // so only the visible chunks are drawn. One tile = the whole checker until #20 adds an atlas.
    using clock = std::chrono::steady_clock;
    render::Tilemap floorMap;
    auto buildFloor = [&](render::TextureId tex, uint32_t texW, uint32_t texH) {
        render::Tileset ts;
        ts.texture = tex;
        ts.textureW = ts.tileW = texW ? texW : 1;
        ts.textureH = ts.tileH = texH ? texH : 1;
        floorMap.create(128, 64, ts, {1, 1}, 16);
        floorMap.setOrigin({-64, -32});
        floorMap.color = render::packGrey(0.22f);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                if ((x + y) % 11 != 0) floorMap.setTile(x, y, render::makeTile(0, x & 1, y & 1));   // diagonal gaps
    };
    buildFloor(render::kWhiteTexture, 1, 1);
    DemoStats stats;

    // Touch: drag pans the demo camera (with fling), pinch zooms around the fingers,
    // double tap resets the view.
    input::GestureConfig gestureCfg;
    input::InputMap inputMap(gestureCfg);
    const input::ActionId actPan = inputMap.bind("pan", {input::BindingKind::Drag, input::Region::full()});
    const input::ActionId actZoom = inputMap.bind("zoom", {input::BindingKind::Pinch, input::Region::full()});
    const input::ActionId actReset = inputMap.bind("reset", {input::BindingKind::DoubleTap, input::Region::full()});
    std::vector<input::TouchEvent> touchEvents;
    touchEvents.reserve(256);
    vec2 camOffset{};          // world units added to the layout camera's center
    float camZoom = 1.0f;
    vec2 flingVelocity{};      // world units per second
    auto lastFrame = clock::now();

    const auto start = clock::now();
    for (;;) {
        ANativeWindow* window = nullptr;
        bool windowChanged = false, sizeChanged = false;
        render::Insets insets;
        {
            std::unique_lock lock(mutex_);
            // Sleep (zero CPU) when paused or without a surface: lean by default.
            cv_.wait(lock, [&] { return quit_ || (!paused_ && window_) || (windowDirty_ && !window_); });
            if (quit_) break;
            windowChanged = windowDirty_;
            sizeChanged = sizeDirty_;
            windowDirty_ = sizeDirty_ = false;
            window = window_;
            insets = insets_;
            if (densityDirty_) {
                gestureCfg.density = density_;
                inputMap.setConfig(gestureCfg);
                densityDirty_ = false;
            }
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
                textures.update();
                if (!checkerResolved && textures.state(checker) != assets::TextureCache::State::Loading) {
                    if (const assets::TextureData* d = textures.get(checker)) {
                        checkerGpu = renderer_->createTexture(*d);
                        buildFloor(checkerGpu, d->width, d->height);
                        MGE_LOGI("Demo texture %s: %s %ux%u, %u mips", kCheckerPath, assets::formatName(d->format),
                                 d->width, d->height, d->mipCount);
                    } else {
                        MGE_LOGW("Demo texture %s failed to load, sprites stay white", kCheckerPath);
                    }
                    checkerResolved = true;
                }

                const auto t0 = clock::now();
                const float t = std::chrono::duration<float>(t0 - start).count();
                uint32_t vw = 0, vh = 0;
                renderer_->viewportSize(vw, vh);
                // Cheap to recompute every frame; picks up rotation and inset changes for free.
                layout = render::ScreenLayout::compute(design, vw, vh, insets);

                // Input: everything the UI thread queued since the last frame.
                const float dt = min(std::chrono::duration<float>(t0 - lastFrame).count(), 0.1f);
                lastFrame = t0;
                touchEvents.clear();
                touches_.drain(touchEvents);
                inputMap.setSurfaceSize(layout.surfaceW, layout.surfaceH);
                inputMap.update(touchEvents,
                                std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count());
                auto viewOf = [&](vec2 offset, float zoom) {
                    render::ScreenLayout v = layout;
                    v.camera = render::Camera2D{layout.camera.center + offset, layout.camera.halfExtent}.zoomed(zoom);
                    return v;
                };
                const float worldPerPx = 1.0f / max(layout.pixelsPerUnit * camZoom, 1e-6f);
                if (inputMap.action(actReset).pressed) {
                    camOffset = {};
                    camZoom = 1.0f;
                    flingVelocity = {};
                }
                if (const input::ActionState& pan = inputMap.action(actPan); pan.held || pan.released) {
                    camOffset -= vec2{pan.value.x, -pan.value.y} * worldPerPx;   // content follows the finger
                    flingVelocity = {};
                }
                for (const input::Gesture& g : inputMap.gestures()) {
                    if (g.type == input::GestureType::DragEnd && !g.cancelled)
                        flingVelocity = -vec2{g.velocity.x, -g.velocity.y} * worldPerPx;
                }
                if (const input::ActionState& zoom = inputMap.action(actZoom); zoom.held || zoom.released) {
                    // Keep the world point under the fingers fixed while zooming, then follow their pan.
                    for (const input::Gesture& g : inputMap.gestures()) {
                        if (g.type != input::GestureType::Pinch) continue;
                        const vec2 before = viewOf(camOffset, camZoom).screenToWorld(g.pos - g.delta);
                        camZoom = clamp(camZoom * g.scale, 0.25f, 8.0f);
                        const vec2 after = viewOf(camOffset, camZoom).screenToWorld(g.pos);
                        camOffset += before - after;
                    }
                    flingVelocity = {};
                }
                if (lengthSq(flingVelocity) > 1e-4f) {
                    camOffset += flingVelocity * dt;
                    flingVelocity *= std::exp(-4.0f * dt);   // ~0.25 s decay
                }
                layout = viewOf(camOffset, camZoom);
                batcher.begin(layout.camera);
                floorMap.draw(batcher);
                for (int y = 0; y < kGridH; ++y) {
                    for (int x = 0; x < kGridW; ++x) {
                        const int i = y * kGridW + x;
                        render::Sprite s;
                        const float speed = 0.4f + 0.15f * float(i % 7);
                        s.world = scene2d::Affine2::trs({float(x) - (kGridW - 1) * 0.5f, float(y) - (kGridH - 1) * 0.5f},
                                                        t * speed * ((i & 1) ? 1.0f : -1.0f), {1, 1});
                        s.size = {0.8f, 0.8f};
                        const float g = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 0.7f + float(x) * 0.15f + float(y) * 0.2f));
                        s.color = render::packGrey(g);
                        s.texture = checkerGpu;
                        batcher.submit(s);
                    }
                }
                batcher.end();
                const auto t1 = clock::now();

                render::FrameInfo fi;
                fi.clearColor = {0.06f, 0.06f, 0.06f, 1};
                fi.letterboxColor = {0, 0, 0, 1};
                fi.viewport = layout.viewport;
                fi.sprites = &batcher;
                if (!renderer_->renderFrame(fi)) std::this_thread::sleep_for(std::chrono::milliseconds(8));
                const auto t2 = clock::now();

                ++stats.frames;
                stats.batchMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                stats.frameMs += std::chrono::duration<double, std::milli>(t2 - t0).count();
                if (t2 - stats.since >= std::chrono::seconds(5)) {
                    const double secs = std::chrono::duration<double>(t2 - stats.since).count();
                    const render::RenderStats rs = renderer_->lastFrameStats();
                    const render::SpriteStats& ss = batcher.stats();
                    MGE_LOGI("Layout: surface %ux%u, viewport %d,%d %ux%u, %.2f px/unit, pixel scale %u, "
                             "safe world (%.2f,%.2f)-(%.2f,%.2f)",
                             layout.surfaceW, layout.surfaceH, layout.viewport.x, layout.viewport.y,
                             layout.viewport.w, layout.viewport.h, double(layout.pixelsPerUnit), layout.pixelScale,
                             double(layout.safeWorld.min.x), double(layout.safeWorld.min.y),
                             double(layout.safeWorld.max.x), double(layout.safeWorld.max.y));
                    MGE_LOGI("Tilemap: %u chunks visible, %u tiles", floorMap.stats().chunksVisible, floorMap.stats().tilesDrawn);
                    MGE_LOGI("Stats: %.1f fps | draws %u | sprites %u drawn, %u culled | textures %u (%.1f KiB) | "
                             "cpu batch %.3f ms, frame %.2f ms (incl. vsync wait)",
                             double(stats.frames) / secs, rs.drawCalls, rs.sprites, ss.culled, rs.textures,
                             double(rs.textureBytes) / 1024.0, stats.batchMs / stats.frames,
                             stats.frameMs / stats.frames);
                    stats = DemoStats{};
                    stats.since = t2;
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (window) ANativeWindow_release(window);
    }
    textures.release(checker);
    if (renderer_) {
        renderer_->waitIdle();
        renderer_->destroyTexture(checkerGpu);
        if (attached_) { renderer_->detachWindow(); ANativeWindow_release(attached_); attached_ = nullptr; }
        delete renderer_;
        renderer_ = nullptr;
    }
    textures.waitAll();
    jobs::shutdown();
    MGE_LOGI("Render thread stopped");
}

}
