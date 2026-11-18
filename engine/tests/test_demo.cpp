#include "test_main.h"
#include "mge/demo/demo_scene.h"
#include "mge/perf/frame_stats.h"
#include <cmath>
#include <cstring>
#include <string>
using namespace mge;

TEST(perf_profiler_percentiles) {
    perf::FrameProfiler p(100);
    for (int i = 1; i <= 100; ++i) p.record({float(i), float(i) * 0.5f, 0, 0, 0});
    const perf::Percentiles f = p.stats(perf::Section::Frame);
    CHECK(std::fabs(f.avg - 50.5f) < 1e-3f && f.p50 == 50.0f && f.p95 == 95.0f && f.p99 == 99.0f && f.max == 100.0f);
    CHECK(p.stats(perf::Section::Cpu).max == 50.0f);
    CHECK(p.framesOver(16.7f) == 84);
    CHECK(p.recent(0).frameMs == 100.0f && p.recent(99).frameMs == 1.0f && p.recent(100).frameMs == 0.0f);
    // The ring keeps the newest `capacity` samples.
    for (int i = 0; i < 50; ++i) p.record({1000.0f, 0, 0, 0, 0});
    CHECK(p.count() == 100 && p.totalFrames() == 150 && p.stats(perf::Section::Frame).p50 == 100.0f &&
          p.stats(perf::Section::Frame).p95 == 1000.0f);
    p.reset();
    CHECK(p.count() == 0 && p.stats(perf::Section::Frame).max == 0.0f);
}

TEST(perf_memory_and_report) {
    const perf::MemoryUsage m = perf::processMemory();
    CHECK(m.rssBytes > 0 && m.virtualBytes >= m.rssBytes);
    perf::BenchReport r;
    r.backend = "host";
    r.frames = 600;
    r.fps = 60.0;
    r.frame.p95 = 12.5f;
    r.drawCalls = 4;
    const std::string line = perf::formatReport(r), json = perf::toJson(r);
    CHECK(line.find("draws 4") != std::string::npos);
    CHECK(json.front() == '{' && json.back() == '}');
    CHECK(json.find("\"frameMs\":{\"avg\":0.000,\"p50\":0.000,\"p95\":12.500") != std::string::npos);
    CHECK(json.find("\"drawCalls\":4") != std::string::npos);
}

TEST(demo_scene_runs_and_stays_in_bounds) {
    demo::DemoScene scene;
    demo::DemoConfig cfg;
    cfg.maxBodies = 300;
    cfg.spawnPerSecond = 60.0f;
    scene.create(cfg);
    const render::Rect b = scene.bounds();
    CHECK(b.size().x == 48.0f && b.size().y == 28.0f);
    for (int i = 0; i < 60 * 12; ++i) scene.update(1.0f / 60.0f);
    const demo::DemoStats& s = scene.stats();
    CHECK(s.spawned == 300 && s.recycled == 0);   // nothing escaped the level
    CHECK(s.bodies == 300 + 4);                   // + ground, goal, ramp, platform
    CHECK(s.flashes > 0);
    CHECK(s.inGoal > 0);                          // sensor events reached the ECS
    int outside = 0;
    scene.ecs().each<physics2d::RigidBody2D, scene2d::Transform2D>([&](ecs::Entity, physics2d::RigidBody2D&,
                                                                        scene2d::Transform2D& t) {
        if (!b.contains(t.position)) ++outside;
    });
    CHECK(outside == 0);

    // Drawing: tilemaps and every sprite go through one batcher.
    render::SpriteBatcher batcher;
    const render::ScreenLayout layout = render::ScreenLayout::compute(scene.design(), 2400, 1080);
    batcher.begin(layout.camera);
    scene.draw(batcher);
    batcher.end();
    CHECK(scene.stats().spritesSubmitted == 302);   // bodies + ramp + platform
    CHECK(scene.stats().tilesDrawn > 0);
    CHECK(batcher.stats().batches == 4);   // one texture, four layers: backdrop, level, ramp+platform, bodies

    // Taps beyond the cap recycle the oldest bodies; a long press pushes things away.
    scene.spawn({0, 8}, 10);
    CHECK(scene.stats().recycled >= 1 && scene.physics().world().bodyCount() <= 304);
    scene.explode({0, -10}, 6.0f, 12.0f);
    scene.update(1.0f / 60.0f);
}

TEST(demo_scene_deterministic) {
    demo::DemoScene a, b;
    demo::DemoConfig cfg;
    cfg.maxBodies = 120;
    a.create(cfg);
    b.create(cfg);
    for (int i = 0; i < 300; ++i) {
        a.update(1.0f / 60.0f);
        b.update(1.0f / 60.0f);
    }
    bool same = true;
    a.physics().world().eachBody([&](physics2d::BodyId id) {
        const physics2d::Transform2 x = a.physics().world().transform(id), y = b.physics().world().transform(id);
        same &= std::memcmp(&x, &y, sizeof x) == 0;
    });
    CHECK(same);
}

TEST(demo_frame_graph_and_atlas) {
    demo::DemoScene scene;
    scene.create();
    scene.setAtlas(7, 128, 128);   // any non-white id: tiles and sprites switch to atlas UVs
    for (int i = 0; i < 30; ++i) scene.update(1.0f / 60.0f);
    render::SpriteBatcher batcher;
    const render::ScreenLayout layout = render::ScreenLayout::compute(scene.design(), 1920, 1080);
    perf::FrameProfiler prof(120);
    for (int i = 0; i < 120; ++i) prof.record({i % 10 == 0 ? 40.0f : 10.0f, 0, 0, 0, 0});
    batcher.begin(layout.camera);
    scene.draw(batcher);
    const uint32_t before = batcher.stats().submitted;
    demo::drawFrameGraph(batcher, layout.safeWorld, prof, 16.667f, 120);
    batcher.end();
    CHECK(batcher.stats().submitted == before + 122);   // panel + 120 bars + budget line
    bool atlasBatch = false;
    for (const render::DrawBatch& d : batcher.batches()) atlasBatch |= d.texture == 7;
    CHECK(atlasBatch);
    CHECK(scene.level().tileset().tileW == 32);
}
