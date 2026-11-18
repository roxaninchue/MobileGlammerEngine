// mge_bench: runs the M1 demo scene headless and prints the same report the device logs.
//   mge_bench [bodies=600] [seconds=20] [json-path]
// Simulates at a fixed 60 Hz (no vsync to wait on, so frameMs == cpuMs) and times update +
// batching per frame. CI runs it after the tests so regressions show up in the log.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "mge/demo/demo_scene.h"
#include "mge/jobs.h"
#include "mge/log.h"
#include "mge/perf/frame_stats.h"

int main(int argc, char** argv) {
    using namespace mge;
    const uint32_t bodies = argc > 1 ? uint32_t(std::atoi(argv[1])) : 600;
    const float seconds = argc > 2 ? float(std::atof(argv[2])) : 20.0f;
    const char* jsonPath = argc > 3 ? argv[3] : nullptr;

    jobs::init();
    demo::DemoScene scene;
    demo::DemoConfig cfg;
    cfg.maxBodies = bodies;
    cfg.spawnPerSecond = float(bodies) / 8.0f;   // full after ~8 s
    scene.create(cfg);
    scene.setAtlas(1, 128, 128);
    render::SpriteBatcher batcher;
    const render::ScreenLayout layout = render::ScreenLayout::compute(scene.design(), 2400, 1080);
    perf::FrameProfiler prof(uint32_t(seconds * 60.0f) + 1);

    using clock = std::chrono::steady_clock;
    const int frames = int(seconds * 60.0f);
    const auto start = clock::now();
    for (int i = 0; i < frames; ++i) {
        const auto t0 = clock::now();
        scene.update(1.0f / 60.0f);
        const auto t1 = clock::now();
        batcher.begin(layout.camera);
        scene.draw(batcher);
        batcher.end();
        const auto t2 = clock::now();
        perf::FrameSample s;
        s.batchMs = std::chrono::duration<float, std::milli>(t2 - t1).count();
        s.physicsMs = scene.stats().physicsMs;
        s.cpuMs = s.frameMs = std::chrono::duration<float, std::milli>(t2 - t0).count();
        prof.record(s);
    }
    const double wall = std::chrono::duration<double>(clock::now() - start).count();

    perf::BenchReport r;
    r.backend = "host (no GPU)";
    r.surfaceW = layout.surfaceW;
    r.surfaceH = layout.surfaceH;
    r.seconds = wall;
    r.frames = prof.totalFrames();
    r.fps = double(r.frames) / wall;
    r.frame = prof.stats(perf::Section::Frame);
    r.cpu = prof.stats(perf::Section::Cpu);
    r.physics = prof.stats(perf::Section::Physics);
    r.batch = prof.stats(perf::Section::Batch);
    r.framesOverBudget = prof.framesOver(r.budgetMs);
    r.drawCalls = batcher.stats().batches;
    r.spritesDrawn = batcher.stats().drawn;
    r.spritesCulled = batcher.stats().culled;
    r.tilesDrawn = scene.stats().tilesDrawn;
    r.entities = scene.stats().entities;
    r.bodies = scene.stats().bodies;
    r.awakeBodies = scene.stats().awakeBodies;
    r.contacts = scene.stats().contacts;
    r.ecsBytes = scene.stats().ecsBytes;
    r.physicsBytes = scene.stats().physicsBytes;
    r.rssBytes = perf::processMemory().rssBytes;
    std::printf("%s\n", perf::formatReport(r).c_str());
    if (jsonPath) {
        if (FILE* f = std::fopen(jsonPath, "w")) {
            std::fprintf(f, "%s\n", perf::toJson(r).c_str());
            std::fclose(f);
        }
    }
    jobs::shutdown();
    return 0;
}
