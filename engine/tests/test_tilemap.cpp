#include "test_main.h"
#include "mge/render/tilemap.h"
#include <cmath>
using namespace mge;
using namespace mge::render;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static Tileset sheet() {   // 4x2 tiles of 16 px, 1 px margin, 2 px spacing -> 1+4*16+3*2+1 = 72 wide
    Tileset ts; ts.texture = 7; ts.textureW = 72; ts.textureH = 36; ts.tileW = ts.tileH = 16;
    ts.margin = 1; ts.spacing = 2; ts.inset = 0; return ts;
}

TEST(tileset_uvs) {
    Tileset ts = sheet();
    CHECK(ts.columns() == 4 && ts.rows() == 2 && ts.count() == 8);
    vec4 uv = ts.uvRect(5);   // col 1, row 1
    CHECK(near(uv.x, 19.0f / 72) && near(uv.y, 19.0f / 36) && near(uv.z, 35.0f / 72) && near(uv.w, 35.0f / 36));
    ts.inset = 0.5f;
    vec4 in = ts.uvRect(0);
    CHECK(near(in.x, 1.5f / 72) && near(in.z, 16.5f / 72));
    CHECK(makeTile(0) == 1 && (makeTile(3, true) & kTileFlipX) && (makeTile(3, true) & kTileIdMask) == 4);
}

TEST(tilemap_chunks_and_dirty) {
    Tilemap m;
    m.create(40, 20, sheet(), {1, 1}, 16);   // 3x2 chunks, the last ones partial
    CHECK(m.chunksX() == 3 && m.chunksY() == 2);
    m.fill(0, 0, 39, 19, makeTile(2));
    m.setTile(-1, 3, makeTile(1));           // ignored
    CHECK(m.tile(-1, 3) == kEmptyTile && m.tile(39, 19) == makeTile(2));
    SpriteBatcher b;
    Camera2D all{{20, 10}, {30, 30}};
    b.begin(all); m.draw(b); b.end();
    CHECK(m.stats().chunksVisible == 6 && m.stats().chunksRebuilt == 6 && m.stats().tilesDrawn == 800);
    CHECK(m.chunkInstanceCount(0, 0) == 256 && m.chunkInstanceCount(2, 1) == 8 * 4);
    CHECK(b.batches().size() == 1 && b.batches()[0].texture == 7 && b.batches()[0].layer == -100);
    // Second frame: everything cached.
    b.begin(all); m.draw(b); b.end();
    CHECK(m.stats().chunksRebuilt == 0 && b.stats().drawn == 800);
    // One edit dirties only its chunk; same-value writes dirty nothing.
    m.setTile(17, 3, kEmptyTile);
    m.setTile(5, 5, makeTile(2));
    CHECK(m.chunkDirty(1, 0) && !m.chunkDirty(0, 0));
    b.begin(all); m.draw(b); b.end();
    CHECK(m.stats().chunksRebuilt == 1 && m.chunkInstanceCount(1, 0) == 255 && b.stats().drawn == 799);
    // Tint change re-dirties all.
    m.color = 0xFF00FF00u;
    b.begin(all); m.draw(b); b.end();
    CHECK(m.stats().chunksRebuilt == 6 && b.instances()[0].color == 0xFF00FF00u);
}

TEST(tilemap_visible_chunks_only) {
    Tilemap m;
    m.create(256, 256, sheet(), {1, 1}, 16);   // 16x16 chunks
    m.fill(0, 0, 255, 255, makeTile(0));
    SpriteBatcher b;
    Camera2D cam{{40, 40}, {8, 4.5f}};         // x 32..48, y 35.5..44.5
    b.begin(cam); m.draw(b); b.end();
    // x: chunks 2..3 (48 is chunk 3's first column), y: chunk 2 only.
    CHECK(m.stats().chunksVisible == 2 && m.stats().chunksRebuilt == 2);
    CHECK(b.stats().drawn == 512);
    Camera2D outside{{-50, -50}, {8, 4.5f}};
    b.begin(outside); m.draw(b); b.end();
    CHECK(m.stats().chunksVisible == 0 && b.stats().drawn == 0);
}

TEST(tilemap_geometry_and_flip) {
    Tilemap m;
    m.create(4, 4, sheet(), {0.5f, 0.5f}, 2);
    m.setOrigin({-1, -1});
    m.setTile(1, 2, makeTile(0, true, false));
    int32_t tx, ty;
    m.worldToTile({-0.25f, 0.1f}, tx, ty);
    CHECK(tx == 1 && ty == 2);
    m.worldToTile({-1.1f, 0}, tx, ty);
    CHECK(tx == -1);
    Rect r = m.tileBounds(1, 2);
    CHECK(near(r.min.x, -0.5f) && near(r.min.y, 0) && near(r.max.x, 0));
    SpriteBatcher b;
    b.begin(Camera2D{{0, 0}, {5, 5}}); m.draw(b); b.end();
    CHECK(b.instances().size() == 1);
    const SpriteInstance& in = b.instances()[0];
    CHECK(near(in.origin[0], -0.5f) && near(in.origin[1], 0) && near(in.axisX[0], 0.5f) && near(in.axisY[1], 0.5f));
    CHECK(in.uv[0] > in.uv[2]);   // flipped horizontally
}

TEST(tilemap_sorts_with_sprites) {
    Tilemap m;
    m.create(2, 1, sheet());
    m.fill(0, 0, 1, 0, makeTile(1));
    m.layer = 0;
    SpriteBatcher b;
    b.begin(Camera2D{{0, 0}, {10, 10}});
    Sprite behind; behind.layer = -1; behind.texture = 7; b.submit(behind);
    Sprite same; same.texture = 7; b.submit(same);   // same layer+texture, submitted before the map
    m.draw(b);
    Sprite front; front.layer = 1; b.submit(front);
    b.end();
    CHECK(b.stats().submitted == 5 && b.stats().drawn == 5);
    const auto& bs = b.batches();
    CHECK(bs.size() == 3);
    CHECK(bs[0].layer == -1 && bs[1].layer == 0 && bs[1].instanceCount == 3 && bs[2].layer == 1);
    // Within layer 0: the sprite first (submitted first), then both tiles.
    CHECK(near(b.instances()[1].origin[0], -0.5f) && near(b.instances()[2].origin[0], 0));
}
