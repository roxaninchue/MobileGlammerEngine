#include "engine_host.h"
#include "mge/log.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <sys/system_properties.h>
#include "mge/jobs.h"
#include "mge/render/camera.h"
#include "mge/render/sprite_batch.h"
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
    DemoStats stats;

    using clock = std::chrono::steady_clock;
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
                batcher.begin(layout.camera);
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
