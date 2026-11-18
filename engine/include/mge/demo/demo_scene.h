#pragma once
// M1 demo scene and benchmark workload (M1 #20). Uses every M1 system together:
//  - ECS entities with Transform2D / WorldTransform2D (TransformSystem2D) and SpriteRenderer.
//  - A level authored as ASCII, turned into a chunked Tilemap (drawn) plus merged static box
//    colliders (physics), over a large background tilemap that is mostly off screen (culling).
//  - physics2d: a spawner pours boxes and balls, a kinematic platform shuttles across, a ramp,
//    a sensor "goal" bin that recolours whatever lands in it (sensor events -> ECS), and hard
//    impacts flash the sprites involved (hit events -> ECS).
//  - Textures from one atlas (textures/demo_atlas.ktx, see tools/make_demo_atlas_ktx.py).
// Pure CPU code in mge_core: the Android host renders it, the host tests and mge_bench run it
// headless with the same numbers.
#include <cstdint>
#include <vector>
#include "mge/ecs.h"
#include "mge/perf/frame_stats.h"
#include "mge/physics2d/physics_system.h"
#include "mge/render/camera.h"
#include "mge/render/sprite_batch.h"
#include "mge/render/sprite_system.h"
#include "mge/render/tilemap.h"
#include "mge/transform2d.h"

namespace mge::demo {

// Atlas layout: 4 x 4 tiles of 32 px, index row-major from the top-left.
enum AtlasTile : uint16_t {
    kTileSolid = 0, kTileSolidTop = 1, kTilePlatform = 2, kTileCrate = 3,
    kTileBall = 4, kTileGoal = 5, kTileBackground = 6, kTileWhite = 7,
};
constexpr const char* kAtlasPath = "textures/demo_atlas.ktx";

struct DemoConfig {
    uint32_t maxBodies = 400;        // the spawner stops here; taps recycle the oldest bodies
    float spawnPerSecond = 40.0f;
    bool autoSpawn = true;
    uint32_t seed = 0x9E3779B9u;
};

struct DemoStats {
    uint32_t entities = 0, bodies = 0, awakeBodies = 0, contacts = 0;
    uint32_t spawned = 0, recycled = 0, inGoal = 0, flashes = 0;
    uint32_t spritesSubmitted = 0, tilesDrawn = 0;
    uint64_t ecsBytes = 0, physicsBytes = 0;
    float physicsMs = 0;             // wall time of the last update's physics advance
};

// Frame-time flash on hard impacts.
struct Flash {
    float time = 0;
    uint32_t baseColor = 0xFFFFFFFFu;
};

// Which atlas tile an entity's sprite shows (kept so setAtlas can remap UVs later).
struct DemoSprite {
    uint16_t tile = kTileCrate;
};

class DemoScene {
public:
    static constexpr int kLevelW = 48, kLevelH = 28;

    void create(const DemoConfig& cfg = {});
    // Switches tiles and sprites to the atlas. Without it everything draws as tinted quads.
    void setAtlas(render::TextureId texture, uint32_t width, uint32_t height);

    // Design area for ScreenLayout: the whole level.
    render::DesignSpec design() const;
    render::Rect bounds() const;

    void spawn(vec2 world, int count);                      // burst at a point (e.g. a tap)
    void explode(vec2 world, float radius, float strength);  // radial impulse (e.g. a long press)

    void update(float dt);
    // Tilemaps + entity sprites; call between batcher.begin() and end().
    void draw(render::SpriteBatcher& batcher);

    const DemoStats& stats() const { return stats_; }
    ecs::World& ecs() { return ecs_; }
    physics2d::PhysicsSystem2D& physics() { return physics_; }
    const render::Tilemap& level() const { return level_; }
    ecs::Entity goal() const { return goal_; }
    ecs::Entity platform() const { return platform_; }

private:
    uint32_t nextRandom();
    float uniform(float lo, float hi);
    vec4 uv(AtlasTile t) const;
    void buildLevel();
    void spawnOne(vec2 p);
    void destroyEntity(ecs::Entity e);

    DemoConfig cfg_;
    ecs::World ecs_;
    physics2d::PhysicsSystem2D physics_;
    scene2d::TransformSystem2D transforms_;
    render::Tilemap level_, background_;
    render::Tileset tileset_;
    std::vector<ecs::Entity> live_;   // spawned bodies, oldest first
    ecs::Entity goal_, platform_, ramp_;
    float time_ = 0, spawnAcc_ = 0;
    uint32_t rng_ = 1;
    DemoStats stats_;
};

// Frame-time graph drawn as sprites in the top-right corner of `area` (world units, e.g. the
// layout's safe rect): one bar per recent frame, green within the budget, amber up to twice
// it, red beyond, with a line at the budget. White-texture sprites on layer 1000.
void drawFrameGraph(render::SpriteBatcher& batcher, const render::Rect& area, const perf::FrameProfiler& profiler,
                    float budgetMs, uint32_t bars = 120);

}  // namespace mge::demo
