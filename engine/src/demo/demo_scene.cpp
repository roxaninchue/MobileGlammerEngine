#include "mge/demo/demo_scene.h"
#include <chrono>
#include <cmath>

namespace mge::demo {

namespace {

using physics2d::BodyDef;
using physics2d::BodyType;
using physics2d::ShapeDef;

// '#' solid, '=' platform, 'G' goal sensor, '.' empty. Row 0 is the top of the level.
constexpr const char* kLevel[DemoScene::kLevelH] = {
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#.....=======......................=======.....#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#..............................................#",
    "#...............=========......................#",
    "#..............................................#",
    "#..............................................#",
    "##.............................................#",
    "##.............................................#",
    "###............................................#",
    "###............................................#",
    "####....................................=======#",
    "####...........................................#",
    "#####...............#......#...................#",
    "#####...............#......#...................#",
    "######..............#GGGGGG#...................#",
    "######..............#GGGGGG#...................#",
    "#######.............#GGGGGG#...................#",
    "################################################",
    "################################################",
};

constexpr vec2 kOrigin{-DemoScene::kLevelW * 0.5f, -DemoScene::kLevelH * 0.5f};

// Colours for spawned bodies (RGBA8, R in the low byte).
constexpr uint32_t kPalette[] = {0xFF4F8BF2u, 0xFF52C77Au, 0xFFE0A33Bu, 0xFFE0576Bu,
                                 0xFFB46FD8u, 0xFF3FC6D9u, 0xFFF2F2F2u, 0xFF6A8CFFu};
constexpr uint32_t kGoalColor = 0xFF2FD7FFu;   // gold

char cell(int x, int row) {
    if (x < 0 || x >= DemoScene::kLevelW || row < 0 || row >= DemoScene::kLevelH) return '.';
    return kLevel[row][x];
}

uint32_t brighten(uint32_t c) {
    vec4 v = render::unpackColor(c);
    return render::packColor({min(1.0f, v.x * 0.4f + 0.6f), min(1.0f, v.y * 0.4f + 0.6f),
                              min(1.0f, v.z * 0.4f + 0.6f), v.w});
}

// Merges cells for which `pick` is true into rectangles: runs per row, then identical runs on
// consecutive rows. Calls emit(x0, row0, x1, row1) inclusive, in ASCII rows (row 0 = top).
template <class Pick, class Emit> void mergeCells(Pick pick, Emit emit) {
    struct Run { int x0, x1, row0, row1; bool open; };
    std::vector<Run> runs;
    for (int row = 0; row < DemoScene::kLevelH; ++row) {
        for (Run& r : runs) r.open = false;
        int x = 0;
        while (x < DemoScene::kLevelW) {
            if (!pick(cell(x, row))) { ++x; continue; }
            int x1 = x;
            while (x1 + 1 < DemoScene::kLevelW && pick(cell(x1 + 1, row))) ++x1;
            bool merged = false;
            for (Run& r : runs) {
                if (r.row1 == row - 1 && r.x0 == x && r.x1 == x1) { r.row1 = row; r.open = true; merged = true; break; }
            }
            if (!merged) runs.push_back({x, x1, row, row, true});
            x = x1 + 1;
        }
    }
    for (const Run& r : runs) emit(r.x0, r.row0, r.x1, r.row1);
}

// World rect of ASCII cells x0..x1, row0..row1 (inclusive).
render::Rect cellRect(int x0, int row0, int x1, int row1) {
    const float yTop = kOrigin.y + float(DemoScene::kLevelH - row0);
    const float yBottom = kOrigin.y + float(DemoScene::kLevelH - 1 - row1);
    return {{kOrigin.x + float(x0), yBottom}, {kOrigin.x + float(x1 + 1), yTop}};
}

}  // namespace

uint32_t DemoScene::nextRandom() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}
float DemoScene::uniform(float lo, float hi) { return lo + (hi - lo) * float(nextRandom() & 0xFFFFFF) / float(0xFFFFFF); }

vec4 DemoScene::uv(AtlasTile t) const {
    return tileset_.texture == render::kWhiteTexture ? vec4{0, 0, 1, 1} : tileset_.uvRect(t);
}

void DemoScene::create(const DemoConfig& cfg) {
    if (goal_.valid()) return;   // created once; make a new DemoScene to start over
    cfg_ = cfg;
    rng_ = cfg.seed ? cfg.seed : 1;
    tileset_ = render::Tileset{};
    buildLevel();
}

void DemoScene::buildLevel() {
    // ---- tilemaps: the level, and a big backdrop that the camera only ever sees part of
    level_.create(kLevelW, kLevelH, tileset_, {1, 1}, 16);
    level_.setOrigin(kOrigin);
    level_.layer = -100;
    background_.create(kLevelW * 4, kLevelH * 4, tileset_, {1, 1}, 16);
    background_.setOrigin(kOrigin - vec2{float(kLevelW) * 1.5f, float(kLevelH) * 1.5f});
    background_.layer = -200;
    background_.color = render::packGrey(0.16f);
    setAtlas(render::kWhiteTexture, 1, 1);

    // ---- static colliders from the same map
    const ecs::Entity ground = ecs_.create();
    BodyDef gd;
    gd.type = BodyType::Static;
    const physics2d::BodyId groundBody = physics_.attach(ecs_, ground, gd);
    mergeCells([](char c) { return c == '#' || c == '='; }, [&](int x0, int r0, int x1, int r1) {
        const render::Rect r = cellRect(x0, r0, x1, r1);
        ShapeDef sd;
        sd.shape = physics2d::makeBox(r.size().x * 0.5f, r.size().y * 0.5f, r.center(), 0.0f);
        sd.friction = 0.7f;
        physics_.world().createShape(groundBody, sd);
    });

    // ---- goal: a sensor filling the bin; whatever enters turns gold
    goal_ = ecs_.create();
    const physics2d::BodyId goalBody = physics_.attach(ecs_, goal_, gd);
    mergeCells([](char c) { return c == 'G'; }, [&](int x0, int r0, int x1, int r1) {
        const render::Rect r = cellRect(x0, r0, x1, r1);
        ShapeDef sd;
        sd.shape = physics2d::makeBox(r.size().x * 0.5f, r.size().y * 0.5f, r.center(), 0.0f);
        sd.isSensor = true;
        physics_.world().createShape(goalBody, sd);
    });

    // ---- a ramp on the right that rolls things towards the bin
    ramp_ = ecs_.create();
    BodyDef rd = gd;
    rd.position = {12.0f, -8.6f};
    rd.angle = 0.30f;
    ShapeDef rs;
    rs.shape = physics2d::makeBox(5.0f, 0.25f);
    rs.friction = 0.3f;
    physics_.attach(ecs_, ramp_, rd, rs);
    ecs_.add<render::SpriteRenderer>(ramp_, render::SpriteRenderer{{10.0f, 0.5f}, {0.5f, 0.5f}, uv(kTilePlatform),
                                                                   render::packGrey(0.75f), tileset_.texture, -50, 1});
    ecs_.add<DemoSprite>(ramp_, DemoSprite{kTilePlatform});

    // ---- kinematic platform shuttling under the top shelves
    platform_ = ecs_.create();
    BodyDef pd;
    pd.type = BodyType::Kinematic;
    pd.position = {0.0f, 3.5f};
    ShapeDef ps;
    ps.shape = physics2d::makeBox(3.0f, 0.25f);
    physics_.attach(ecs_, platform_, pd, ps);
    ecs_.add<render::SpriteRenderer>(platform_, render::SpriteRenderer{{6.0f, 0.5f}, {0.5f, 0.5f}, uv(kTilePlatform),
                                                                       0xFFE8C070u, tileset_.texture, -50, 1});
    ecs_.add<DemoSprite>(platform_, DemoSprite{kTilePlatform});
}

void DemoScene::setAtlas(render::TextureId texture, uint32_t width, uint32_t height) {
    render::Tileset ts;
    ts.texture = texture;
    if (texture == render::kWhiteTexture) {
        ts.textureW = ts.textureH = ts.tileW = ts.tileH = 1;
        ts.inset = 0.0f;
    } else {
        ts.textureW = width;
        ts.textureH = height;
        ts.tileW = width / 4;
        ts.tileH = height / 4;
    }
    tileset_ = ts;
    level_.setTileset(ts);
    background_.setTileset(ts);
    const bool atlas = texture != render::kWhiteTexture;
    // Level tiles: solid tops get the grass-edge tile; the bin reads as goal tiles.
    for (int row = 0; row < kLevelH; ++row) {
        for (int x = 0; x < kLevelW; ++x) {
            const char c = cell(x, row);
            const int ty = kLevelH - 1 - row;
            render::Tile t = render::kEmptyTile;
            if (c == '#') t = render::makeTile(atlas && cell(x, row - 1) != '#' ? kTileSolidTop : kTileSolid);
            else if (c == '=') t = render::makeTile(atlas ? kTilePlatform : kTileSolid);
            else if (c == 'G') t = render::makeTile(atlas ? kTileGoal : kTileSolid);
            level_.setTile(x, ty, t);
        }
    }
    level_.color = atlas ? 0xFFFFFFFFu : render::packGrey(0.45f);
    for (uint32_t y = 0; y < background_.height(); ++y)
        for (uint32_t x = 0; x < background_.width(); ++x)
            background_.setTile(int32_t(x), int32_t(y),
                                ((x + y) % 7 == 0) ? render::kEmptyTile : render::makeTile(kTileBackground, x & 1, y & 1));
    // Existing sprites follow the new texture.
    ecs_.each<DemoSprite, render::SpriteRenderer>([&](ecs::Entity, DemoSprite& d, render::SpriteRenderer& sr) {
        sr.texture = texture;
        sr.uvRect = uv(AtlasTile(d.tile));
    });
}

render::Rect DemoScene::bounds() const { return {kOrigin, kOrigin + vec2{float(kLevelW), float(kLevelH)}}; }

render::DesignSpec DemoScene::design() const {
    render::DesignSpec d;
    d.area = bounds();
    d.mode = render::ScaleMode::Expand;
    d.artPixelsPerUnit = 32.0f;
    return d;
}

void DemoScene::spawnOne(vec2 p) {
    if (live_.size() >= cfg_.maxBodies && !live_.empty()) {
        destroyEntity(live_.front());
        live_.erase(live_.begin());
        ++stats_.recycled;
    }
    const ecs::Entity e = ecs_.create();
    BodyDef bd;
    bd.position = p;
    bd.angle = uniform(-kPi, kPi);
    bd.angularVelocity = uniform(-2.0f, 2.0f);
    const uint32_t color = kPalette[nextRandom() % (sizeof(kPalette) / sizeof(kPalette[0]))];
    ShapeDef sd;
    vec2 size;
    bool ball = (nextRandom() % 3) == 0;
    if (ball) {
        const float r = uniform(0.3f, 0.5f);
        sd.shape = physics2d::Circle{{0, 0}, r};
        sd.restitution = 0.35f;
        size = {2 * r, 2 * r};
    } else {
        const float hx = uniform(0.3f, 0.6f), hy = uniform(0.3f, 0.6f);
        sd.shape = physics2d::makeBox(hx, hy);
        sd.restitution = 0.05f;
        size = {2 * hx, 2 * hy};
    }
    sd.friction = 0.6f;
    physics_.attach(ecs_, e, bd, sd);
    ecs_.add<render::SpriteRenderer>(e, render::SpriteRenderer{size, {0.5f, 0.5f}, uv(ball ? kTileBall : kTileCrate),
                                                               color, tileset_.texture, 0, 1});
    ecs_.add<Flash>(e, Flash{0.0f, color});
    ecs_.add<DemoSprite>(e, DemoSprite{uint16_t(ball ? kTileBall : kTileCrate)});
    live_.push_back(e);
    ++stats_.spawned;
}

void DemoScene::spawn(vec2 world, int count) {
    const render::Rect b = bounds();
    for (int i = 0; i < count; ++i) {
        vec2 p = world + vec2{uniform(-1.0f, 1.0f), uniform(-1.0f, 1.0f)};
        p.x = clamp(p.x, b.min.x + 1.6f, b.max.x - 1.6f);
        p.y = clamp(p.y, b.min.y + 3.0f, b.max.y - 1.0f);
        if (physics_.world().testPoint(p)) continue;   // never inside a wall
        spawnOne(p);
    }
}

void DemoScene::explode(vec2 world, float radius, float strength) {
    physics2d::World& w = physics_.world();
    std::vector<physics2d::BodyId> hit;
    w.queryAABB(physics2d::AABB{world - vec2{radius, radius}, world + vec2{radius, radius}}, [&](physics2d::ShapeId s) {
        const physics2d::BodyId b = w.shapeBody(s);
        if (w.bodyType(b) == BodyType::Dynamic) hit.push_back(b);
        return true;
    });
    for (physics2d::BodyId b : hit) {
        const vec2 d = w.worldCenter(b) - world;
        const float dist = length(d);
        if (dist > radius) continue;
        const vec2 dir = dist > 1e-4f ? d / dist : vec2{0, 1};
        w.applyLinearImpulseToCenter(b, dir * (strength * (1.0f - dist / radius) * w.mass(b)));
    }
}

void DemoScene::destroyEntity(ecs::Entity e) {
    physics_.detach(ecs_, e);
    ecs_.destroy(e);
}

void DemoScene::update(float dt) {
    dt = clamp(dt, 0.0f, 0.1f);
    time_ += dt;

    // Platform: smooth back-and-forth, 7 m either side, 8 s period (driven by velocity so the
    // solver sees it move and carries passengers).
    if (const physics2d::BodyId pb = physics_.bodyOf(ecs_, platform_); pb.valid()) {
        const float w = 2.0f * kPi / 8.0f;
        physics_.world().setLinearVelocity(pb, {7.0f * w * std::cos(w * time_), 0.0f});
    }

    if (cfg_.autoSpawn) {
        spawnAcc_ += dt * cfg_.spawnPerSecond;
        const render::Rect b = bounds();
        while (spawnAcc_ >= 1.0f) {
            spawnAcc_ -= 1.0f;
            if (live_.size() >= cfg_.maxBodies) { spawnAcc_ = 0.0f; break; }
            spawnOne({uniform(b.min.x + 3.0f, b.max.x - 3.0f), b.max.y - 1.5f});
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    physics_.update(ecs_, dt);
    stats_.physicsMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Physics events into the ECS: impacts flash, the goal recolours its visitors.
    for (const physics2d::CollisionEvent2D& ev : physics_.events()) {
        if (ev.type == physics2d::CollisionEventType::Hit && ev.speed > 3.0f) {
            for (ecs::Entity e : {ev.a, ev.b}) {
                if (Flash* f = ecs_.get<Flash>(e)) {
                    f->time = 0.15f;
                    ++stats_.flashes;
                }
            }
        } else if (ev.type == physics2d::CollisionEventType::SensorBegin && ev.a == goal_) {
            if (Flash* f = ecs_.get<Flash>(ev.b); f && f->baseColor != kGoalColor) {
                f->baseColor = kGoalColor;
                ++stats_.inGoal;
            }
        }
    }
    ecs_.each<Flash, render::SpriteRenderer>([&](ecs::Entity, Flash& f, render::SpriteRenderer& sr) {
        f.time = max(0.0f, f.time - dt);
        const uint32_t c = f.time > 0.0f ? brighten(f.baseColor) : f.baseColor;
        if (sr.color != c) sr.color = c;
    });

    // Anything that escaped the level (it should not) is recycled.
    const float killY = bounds().min.y - 5.0f;
    for (size_t i = 0; i < live_.size();) {
        const scene2d::Transform2D* t = ecs_.get<scene2d::Transform2D>(live_[i]);
        if (t && t->position.y < killY) {
            destroyEntity(live_[i]);
            live_.erase(live_.begin() + std::ptrdiff_t(i));
            ++stats_.recycled;
        } else {
            ++i;
        }
    }

    transforms_.update(ecs_);

    const physics2d::WorldStats& ws = physics_.world().stats();
    stats_.entities = ecs_.aliveCount();
    stats_.bodies = uint32_t(ws.bodies);
    stats_.awakeBodies = uint32_t(ws.awakeBodies);
    stats_.contacts = uint32_t(ws.contacts);
    stats_.ecsBytes = ecs_.memoryBytes();
    stats_.physicsBytes = ws.memoryBytes;
}

void DemoScene::draw(render::SpriteBatcher& batcher) {
    background_.draw(batcher);
    level_.draw(batcher);
    stats_.tilesDrawn = background_.stats().tilesDrawn + level_.stats().tilesDrawn;
    stats_.spritesSubmitted = render::submitSprites(ecs_, batcher);
}

void drawFrameGraph(render::SpriteBatcher& batcher, const render::Rect& area, const perf::FrameProfiler& profiler,
                    float budgetMs, uint32_t bars) {
    if (bars == 0 || budgetMs <= 0.0f) return;
    const vec2 size = area.size();
    const float w = size.x * 0.32f, h = size.y * 0.12f, margin = min(size.x, size.y) * 0.03f;
    const vec2 origin{area.max.x - margin - w, area.max.y - margin - h};   // bottom-left of the panel
    auto quad = [&](vec2 bottomLeft, vec2 extent, uint32_t color, int16_t layer) {
        render::Sprite s;
        s.world = scene2d::Affine2::trs(bottomLeft, 0.0f, {1, 1});
        s.size = extent;
        s.pivot = {0, 0};
        s.color = color;
        s.layer = layer;
        batcher.submit(s);
    };
    quad(origin, {w, h}, render::packColor({0, 0, 0, 0.55f}), 1000);
    const float fullScaleMs = 2.5f * budgetMs;
    const float barW = w / float(bars);
    const uint32_t n = min(bars, profiler.count());
    for (uint32_t i = 0; i < n; ++i) {
        const float ms = profiler.recent(i).frameMs;
        const float bh = h * clamp(ms / fullScaleMs, 0.02f, 1.0f);
        const uint32_t color = ms <= budgetMs * 1.05f ? 0xFF5BD46Bu : (ms <= 2.0f * budgetMs ? 0xFF2FB8F5u : 0xFF4646F0u);
        quad({origin.x + w - barW * float(i + 1), origin.y}, {barW * 0.8f, bh}, color, 1001);
    }
    quad({origin.x, origin.y + h * (budgetMs / fullScaleMs)}, {w, h * 0.015f}, render::packGrey(1.0f, 0.8f), 1002);
}

}  // namespace mge::demo
