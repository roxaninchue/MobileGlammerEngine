#include "test_main.h"
#include "mge/render/sprite_batch.h"
#include <cmath>
using namespace mge;
using namespace mge::render;
using scene2d::Affine2;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static Camera2D bigCamera() { Camera2D c; c.halfExtent = {1000, 1000}; return c; }

TEST(sprite_instance_transform) {
    static_assert(sizeof(SpriteInstance) == 36);
    Sprite s;
    s.world = Affine2::trs({10, 20}, radians(90), {2, 2});
    s.size = {4, 2};                           // centred pivot by default
    s.uvRect = {0, 0.5f, 1, 1};
    SpriteInstance in = SpriteBatcher::makeInstance(s);
    // x edge (4 wide, scaled 2, rotated 90) points along +y.
    CHECK(near(in.axisX[0], 0) && near(in.axisX[1], 8));
    CHECK(near(in.axisY[0], -4) && near(in.axisY[1], 0));
    // uv0 corner = pivot offset (-2,-1) -> scaled (-4,-2) -> rotated (2,-4) -> +t.
    CHECK(near(in.origin[0], 12) && near(in.origin[1], 16));
    // Opposite corner lands symmetric around the sprite's position.
    CHECK(near(in.origin[0] + in.axisX[0] + in.axisY[0], 8));
    CHECK(near(in.origin[1] + in.axisX[1] + in.axisY[1], 24));
    CHECK(in.uv[0] == 0 && in.uv[1] == 32768 && in.uv[2] == 65535 && in.uv[3] == 65535);
}

TEST(sprite_sort_layer_texture_order) {
    SpriteBatcher b;
    b.begin(bigCamera());
    auto add = [&](int16_t layer, TextureId tex, float x) {
        Sprite s; s.layer = layer; s.texture = tex; s.world.tx = x; b.submit(s);
    };
    add(1, 5, 0); add(0, 7, 1); add(1, 2, 2); add(0, 7, 3); add(-3, 9, 4); add(1, 5, 5);
    b.end();
    const auto& in = b.instances();
    CHECK(in.size() == 6);
    // Expected order: layer -3 (x4), layer 0 tex 7 (x1, x3), layer 1 tex 2 (x2), layer 1 tex 5 (x0, x5).
    const float expect[] = {4, 1, 3, 2, 0, 5};
    for (int i = 0; i < 6; ++i) CHECK(near(in[i].origin[0] + 0.5f, expect[i]));
}

TEST(sprite_batch_merging) {
    SpriteBatcher b;
    b.begin(bigCamera());
    for (int i = 0; i < 100; ++i) { Sprite s; s.texture = TextureId(i % 2 ? 3 : 1); b.submit(s); }
    Sprite top; top.layer = 2; top.texture = 1; b.submit(top);
    b.end();
    const auto& bs = b.batches();
    CHECK(bs.size() == 3);                     // tex 1, tex 3 on layer 0, then tex 1 on layer 2
    CHECK(bs[0].texture == 1 && bs[0].instanceCount == 50 && bs[0].firstInstance == 0);
    CHECK(bs[1].texture == 3 && bs[1].instanceCount == 50 && bs[1].firstInstance == 50);
    CHECK(bs[2].layer == 2 && bs[2].firstInstance == 100 && bs[2].instanceCount == 1);
    CHECK(b.stats().batches == 3 && b.stats().drawn == 101);
}

TEST(sprite_culling) {
    Camera2D cam;
    cam.center = {0, 0};
    cam.halfExtent = {10, 5};
    SpriteBatcher b;
    b.begin(cam);
    Sprite in; in.world.tx = 9;                // overlaps the right edge
    Sprite out; out.world.tx = 20;             // fully off-screen
    Sprite rotated; rotated.size = {30, 0.1f}; // long thin sprite rotated so it crosses the view
    rotated.world = Affine2::trs({0, 12}, radians(90), {1, 1});
    Sprite clear; clear.color = packColor({1, 1, 1, 0});
    b.submit(in); b.submit(out); b.submit(rotated); b.submit(clear);
    b.end();
    CHECK(b.stats().submitted == 4);
    CHECK(b.stats().culled == 2);
    CHECK(b.stats().drawn == 2);
    b.cullingEnabled = false;
    b.begin(cam); b.submit(out); b.end();
    CHECK(b.stats().drawn == 1);
}

TEST(sprite_color_packing) {
    uint32_t p = packColor({1.0f, 0.5f, 0.0f, 0.25f});
    CHECK((p & 0xFF) == 255);                  // R in the lowest byte
    CHECK(((p >> 8) & 0xFF) == 128);
    CHECK(((p >> 16) & 0xFF) == 0);
    CHECK((p >> 24) == 64);
    CHECK(packColor({2, -1, 0, 1}) == 0xFF0000FFu);   // clamped
    CHECK(packGrey(1.0f) == 0xFFFFFFFFu);
    vec4 c = unpackColor(packColor({0.2f, 0.4f, 0.6f, 0.8f}));
    CHECK(std::fabs(c.x - 0.2f) < 0.003f && std::fabs(c.w - 0.8f) < 0.003f);
}
