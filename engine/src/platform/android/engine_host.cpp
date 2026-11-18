#include "engine_host.h"
#include "mge/log.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <sys/system_properties.h>
#include <cstdio>
#include <cstdlib>
#include "mge/demo/demo_scene.h"
#include "mge/jobs.h"
#include "mge/perf/frame_stats.h"
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

// debug.mge.scale = letterbox|expand|crop|fitwidth|fitheight (default expand),
// debug.mge.pixelperfect = 1 to snap to whole screen pixels per texel.
render::DesignSpec demoDesign(render::DesignSpec d) {
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
    return d;
}

int propInt(const char* name, int fallback) {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, v) <= 0) return fallback;
    return std::atoi(v);
}

// Benchmark mode (debug.mge.bench = body count, e.g. 800): fills the scene, lets it run
// kBenchWarmup seconds, measures kBenchMeasure seconds, logs the report and writes
// <files>/bench.json, then keeps running as the normal demo.
constexpr float kBenchWarmup = 10.0f, kBenchMeasure = 20.0f;

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

std::string EngineHost::statsText() {
    std::lock_guard lock(statsMutex_);
    return statsText_;
}

void EngineHost::renderLoop() {
    MGE_LOGI("Render thread started");
    jobs::init();
    renderer_ = render::createBestRenderer();
    if (!renderer_) MGE_LOGE("No usable renderer (Vulkan and GLES3 both unavailable)");
    const char* backendName = !renderer_ ? "none" : (renderer_->backend() == render::Backend::Vulkan ? "Vulkan" : "GLES3");

    assets::TextureCacheConfig cacheCfg;
    cacheCfg.reader = assetReader;
    cacheCfg.user = assets_;
    assets::TextureCache textures(cacheCfg);
    assets::TextureHandle atlas = textures.acquire(demo::kAtlasPath);
    render::TextureId atlasGpu = render::kWhiteTexture;
    bool atlasResolved = false;
    render::SpriteBatcher batcher;

    // M1 demo: the physics playground (see mge/demo/demo_scene.h).
    const int benchBodies = propInt("debug.mge.bench", 0);
    demo::DemoConfig demoCfg;
    if (benchBodies > 0) {
        demoCfg.maxBodies = uint32_t(benchBodies);
        demoCfg.spawnPerSecond = float(benchBodies) / 6.0f;
    }
    demo::DemoScene scene;
    scene.create(demoCfg);
    const render::DesignSpec design = demoDesign(scene.design());
    const bool showGraph = propInt("debug.mge.overlay", 1) != 0;
    render::ScreenLayout layout;
    using clock = std::chrono::steady_clock;

    // Timing: a rolling 10 s window for the overlay and the 5 s log line.
    perf::FrameProfiler profiler(600);
    constexpr float kBudgetMs = 1000.0f / 60.0f;
    auto lastReport = clock::now();
    bool benchDone = benchBodies <= 0;
    double benchClock = 0.0;
    bool benchMeasuring = false;
    perf::FrameProfiler benchProfiler(uint32_t(kBenchMeasure * 125.0f));   // room for 120 Hz panels
    if (benchBodies > 0) MGE_LOGI("Benchmark: %d bodies, %.0f s warm-up, %.0f s measured", benchBodies,
                                  double(kBenchWarmup), double(kBenchMeasure));

    auto makeReport = [&](const perf::FrameProfiler& prof, double seconds) {
        perf::BenchReport r;
        r.backend = backendName;
        r.surfaceW = layout.surfaceW;
        r.surfaceH = layout.surfaceH;
        r.seconds = seconds;
        r.frames = prof.count();
        r.fps = seconds > 0 ? double(prof.count()) / seconds : 0.0;
        r.frame = prof.stats(perf::Section::Frame);
        r.cpu = prof.stats(perf::Section::Cpu);
        r.physics = prof.stats(perf::Section::Physics);
        r.batch = prof.stats(perf::Section::Batch);
        r.render = prof.stats(perf::Section::Render);
        r.budgetMs = kBudgetMs;
        r.framesOverBudget = prof.framesOver(kBudgetMs * 1.05f);
        const render::RenderStats rs = renderer_->lastFrameStats();
        r.drawCalls = rs.drawCalls;
        r.spritesDrawn = rs.sprites;
        r.spritesCulled = batcher.stats().culled;
        r.tilesDrawn = scene.stats().tilesDrawn;
        r.textures = rs.textures;
        r.textureBytes = rs.textureBytes;
        r.entities = scene.stats().entities;
        r.bodies = scene.stats().bodies;
        r.awakeBodies = scene.stats().awakeBodies;
        r.contacts = scene.stats().contacts;
        r.ecsBytes = scene.stats().ecsBytes;
        r.physicsBytes = scene.stats().physicsBytes;
        r.rssBytes = perf::processMemory().rssBytes;
        return r;
    };

    // Touch: drag pans the camera (with fling), pinch zooms around the fingers, double tap
    // resets the view, tap drops a burst of bodies, long press blasts them apart.
    input::GestureConfig gestureCfg;
    input::InputMap inputMap(gestureCfg);
    const input::ActionId actPan = inputMap.bind("pan", {input::BindingKind::Drag, input::Region::full()});
    const input::ActionId actZoom = inputMap.bind("zoom", {input::BindingKind::Pinch, input::Region::full()});
    const input::ActionId actReset = inputMap.bind("reset", {input::BindingKind::DoubleTap, input::Region::full()});
    const input::ActionId actSpawn = inputMap.bind("spawn", {input::BindingKind::Tap, input::Region::full()});
    const input::ActionId actBlast = inputMap.bind("blast", {input::BindingKind::LongPress, input::Region::full()});
    std::vector<input::TouchEvent> touchEvents;
    touchEvents.reserve(256);
    vec2 camOffset{};          // world units added to the layout camera's center
    float camZoom = 1.0f;
    vec2 flingVelocity{};      // world units per second
    auto lastFrame = clock::now();
    float lastRenderMs = 0.0f;

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
                if (!atlasResolved && textures.state(atlas) != assets::TextureCache::State::Loading) {
                    if (const assets::TextureData* d = textures.get(atlas)) {
                        atlasGpu = renderer_->createTexture(*d);
                        scene.setAtlas(atlasGpu, d->width, d->height);
                        MGE_LOGI("Demo atlas %s: %s %ux%u, %u mips", demo::kAtlasPath, assets::formatName(d->format),
                                 d->width, d->height, d->mipCount);
                    } else {
                        MGE_LOGW("Demo atlas %s failed to load, drawing tinted quads", demo::kAtlasPath);
                    }
                    atlasResolved = true;
                }

                const auto t0 = clock::now();
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
                if (const input::ActionState& tap = inputMap.action(actSpawn); tap.pressed)
                    scene.spawn(layout.screenToWorld(tap.pos), 8);
                if (const input::ActionState& hold = inputMap.action(actBlast); hold.pressed)
                    scene.explode(layout.screenToWorld(hold.pos), 6.0f, 14.0f);

                // Simulation (fixed-step physics inside) then drawing.
                scene.update(dt);
                const auto t1 = clock::now();
                batcher.begin(layout.camera);
                scene.draw(batcher);
                if (showGraph) {
                    const render::Rect cam = layout.camera.bounds();
                    const render::Rect safe{max(cam.min, layout.safeWorld.min), min(cam.max, layout.safeWorld.max)};
                    demo::drawFrameGraph(batcher, safe, profiler, kBudgetMs);
                }
                batcher.end();
                const auto t2 = clock::now();

                render::FrameInfo fi;
                fi.clearColor = {0.05f, 0.06f, 0.08f, 1};
                fi.letterboxColor = {0, 0, 0, 1};
                fi.viewport = layout.viewport;
                fi.sprites = &batcher;
                if (!renderer_->renderFrame(fi)) std::this_thread::sleep_for(std::chrono::milliseconds(8));
                const auto t3 = clock::now();

                // frameMs runs from this frame's start to the next one's (includes the vsync
                // wait inside renderFrame); cpuMs is our own work before presenting.
                perf::FrameSample sample;
                sample.frameMs = dt * 1000.0f;
                sample.physicsMs = scene.stats().physicsMs;
                sample.batchMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
                sample.renderMs = lastRenderMs;
                sample.cpuMs = std::chrono::duration<float, std::milli>(t2 - t0).count() + lastRenderMs;
                lastRenderMs = std::chrono::duration<float, std::milli>(t3 - t2).count();
                profiler.record(sample);

                if (!benchDone) {
                    benchClock += double(dt);
                    if (!benchMeasuring && benchClock >= kBenchWarmup) {
                        benchMeasuring = true;
                        benchClock = 0.0;
                        benchProfiler.reset();
                        MGE_LOGI("Benchmark: measuring");
                    } else if (benchMeasuring) {
                        benchProfiler.record(sample);
                        if (benchClock >= kBenchMeasure) {
                            const perf::BenchReport r = makeReport(benchProfiler, benchClock);
                            MGE_LOGI("Benchmark result: %s", perf::formatReport(r).c_str());
                            const std::string path = dataPath_ + "/bench.json";
                            if (FILE* f = std::fopen(path.c_str(), "w")) {
                                std::fprintf(f, "%s\n", perf::toJson(r).c_str());
                                std::fclose(f);
                                MGE_LOGI("Benchmark written to %s", path.c_str());
                            }
                            benchDone = true;
                        }
                    }
                }

                if (t3 - lastReport >= std::chrono::milliseconds(500)) {
                    // On-screen readout (polled by the app) twice a second, logcat every 5 s.
                    const perf::Percentiles ft = profiler.stats(perf::Section::Frame);
                    const perf::Percentiles cpu = profiler.stats(perf::Section::Cpu);
                    const render::RenderStats rs = renderer_->lastFrameStats();
                    const perf::MemoryUsage mem = perf::processMemory();
                    char text[512];
                    std::snprintf(text, sizeof text,
                                  "%s  %.0f fps  frame %.1f ms (p95 %.1f)  cpu %.2f ms  physics %.2f ms\n"
                                  "draws %u  sprites %u  bodies %u (%u awake)  contacts %u\n"
                                  "mem rss %.1f MiB  ecs %.0f KiB  physics %.0f KiB  textures %.0f KiB",
                                  backendName, ft.avg > 0 ? 1000.0 / double(ft.avg) : 0.0, double(ft.avg),
                                  double(ft.p95), double(cpu.avg), double(profiler.stats(perf::Section::Physics).avg),
                                  rs.drawCalls, rs.sprites, scene.stats().bodies, scene.stats().awakeBodies,
                                  scene.stats().contacts, double(mem.rssBytes) / (1024.0 * 1024.0),
                                  double(scene.stats().ecsBytes) / 1024.0, double(scene.stats().physicsBytes) / 1024.0,
                                  double(rs.textureBytes) / 1024.0);
                    {
                        std::lock_guard lock(statsMutex_);
                        statsText_ = text;
                    }
                    static int ticks = 0;
                    if (++ticks % 10 == 0) {
                        const double window = double(profiler.count()) * double(ft.avg) / 1000.0;
                        MGE_LOGI("Stats: %s", perf::formatReport(makeReport(profiler, window)).c_str());
                    }
                    lastReport = t3;
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (window) ANativeWindow_release(window);
    }
    textures.release(atlas);
    if (renderer_) {
        renderer_->waitIdle();
        renderer_->destroyTexture(atlasGpu);
        if (attached_) { renderer_->detachWindow(); ANativeWindow_release(attached_); attached_ = nullptr; }
        delete renderer_;
        renderer_ = nullptr;
    }
    textures.waitAll();
    jobs::shutdown();
    MGE_LOGI("Render thread stopped");
}

}
