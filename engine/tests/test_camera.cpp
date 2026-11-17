#include "test_main.h"
#include "mge/render/camera.h"
#include <cmath>
using namespace mge;
using namespace mge::render;

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
static bool near(vec2 a, vec2 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }
static DesignSpec design16x9(ScaleMode m) { DesignSpec d; d.area = {{-8, -4.5f}, {8, 4.5f}}; d.mode = m; return d; }

TEST(camera_screen_world_roundtrip) {
    Camera2D c{{3, -2}, {10, 5}};
    CHECK(near(c.viewportToWorld({0, 0}, 200, 100), {-7, 3}));       // top-left
    CHECK(near(c.viewportToWorld({200, 100}, 200, 100), {13, -7}));  // bottom-right
    CHECK(near(c.viewportToWorld({100, 50}, 200, 100), {3, -2}));
    vec2 w{1.25f, 0.5f};
    CHECK(near(c.viewportToWorld(c.worldToViewport(w, 200, 100), 200, 100), w));
    Camera2D z = c.zoomed(2);
    CHECK(near(z.halfExtent, {5, 2.5f}) && near(z.center, c.center));
}

TEST(camera_letterbox_portrait) {
    // 1080x1920 portrait: 16:9 design fits the width, bars top and bottom.
    ScreenLayout L = ScreenLayout::compute(design16x9(ScaleMode::Letterbox), 1080, 1920);
    CHECK(near(L.pixelsPerUnit, 67.5f));
    CHECK(L.viewport.w == 1080 && L.viewport.h == 608 && L.viewport.x == 0 && L.viewport.y == 656);
    CHECK(near(L.camera.center, {0, 0}));
    CHECK(near(L.camera.halfExtent.x, 8) && near(L.camera.halfExtent.y, 4.5f, 0.01f));
    // Viewport corners map to the design corners; bars are outside the viewport.
    CHECK(near(L.screenToWorld({0, 656}), {-8, 4.5f}, 0.01f));
    CHECK(near(L.screenToWorld({1080, 1264}), {8, -4.5f}, 0.01f));
    CHECK(!L.screenInViewport({540, 100}) && L.screenInViewport({540, 960}));
}

TEST(camera_expand_and_insets) {
    // 2400x1080 landscape with a 120 px cutout on the left and a 60 px nav bar at the bottom.
    Insets in{120, 0, 0, 60};
    ScreenLayout L = ScreenLayout::compute(design16x9(ScaleMode::Expand), 2400, 1080, in);
    CHECK(L.viewport.x == 0 && L.viewport.y == 0 && L.viewport.w == 2400 && L.viewport.h == 1080);
    // Safe rect is 2280x1020; height limits: 1020 / 9.
    const float s = 1020.0f / 9.0f;
    CHECK(near(L.pixelsPerUnit, s));
    // The design center sits at the safe rect's center (1260, 510) on screen.
    CHECK(near(L.worldToScreen({0, 0}), {1260, 510}, 0.05f));
    // Whole design area is inside the safe world rect, which is inside what the camera shows.
    CHECK(L.safeWorld.min.x <= -8 + 1e-3f && L.safeWorld.max.x >= 8 - 1e-3f);
    CHECK(near(L.safeWorld.min.y, -4.5f, 0.01f) && near(L.safeWorld.max.y, 4.5f, 0.01f));
    Rect view = L.camera.bounds();
    CHECK(view.min.x < L.safeWorld.min.x && view.min.y < L.safeWorld.min.y);
    CHECK(near(view.max.y, L.safeWorld.max.y, 0.01f));   // no top inset: safe edge = screen edge
}

TEST(camera_crop_fit_modes) {
    ScreenLayout crop = ScreenLayout::compute(design16x9(ScaleMode::Crop), 1080, 1920);
    // Covers the screen: height decides, the design's sides are cut.
    CHECK(near(crop.pixelsPerUnit, 1920.0f / 9.0f));
    CHECK(crop.camera.bounds().min.x > -8 && near(crop.camera.halfExtent.y, 4.5f));
    ScreenLayout fw = ScreenLayout::compute(design16x9(ScaleMode::FitWidth), 1080, 1920);
    CHECK(near(fw.camera.halfExtent.x, 8));
    ScreenLayout fh = ScreenLayout::compute(design16x9(ScaleMode::FitHeight), 1920, 1080);
    CHECK(near(fh.camera.halfExtent.y, 4.5f) && near(fh.camera.halfExtent.x, 8));
    // fitHeight helper still matches the old behaviour.
    Camera2D c = Camera2D::fitHeight({0, 0}, 9, 1920, 1080);
    CHECK(near(c.halfExtent, {8, 4.5f}));
    // No surface yet: camera falls back to the design area.
    ScreenLayout none = ScreenLayout::compute(design16x9(ScaleMode::Expand), 0, 0);
    CHECK(near(none.camera.halfExtent, {8, 4.5f}));
}

TEST(camera_pixel_perfect) {
    // 320x180 art (16 texels/unit over 20x11.25 units) on 1920x1080 -> exactly 6x.
    DesignSpec d;
    d.area = {{0, 0}, {20, 11.25f}};
    d.mode = ScaleMode::Letterbox;
    d.pixelPerfect = true;
    d.artPixelsPerUnit = 16;
    ScreenLayout L = ScreenLayout::compute(d, 1920, 1080);
    CHECK(L.pixelScale == 6 && near(L.pixelsPerUnit, 96));
    CHECK(L.viewport.w == 1920 && L.viewport.h == 1080);
    // 2340x1080: still 6x (rounded down), centred with 210 px bars each side.
    L = ScreenLayout::compute(d, 2340, 1080);
    CHECK(L.pixelScale == 6 && L.viewport.x == 210 && L.viewport.w == 1920);
    // 1000x700: 3x, never below 1x.
    L = ScreenLayout::compute(d, 1000, 700);
    CHECK(L.pixelScale == 3);
    L = ScreenLayout::compute(d, 100, 50);
    CHECK(L.pixelScale == 1);
    // Expand + pixel perfect: camera edge lands on the pixel grid.
    d.mode = ScaleMode::Expand;
    L = ScreenLayout::compute(d, 2340, 1081);
    vec2 lo = L.camera.bounds().min * L.pixelsPerUnit;
    CHECK(near(lo.x, std::round(lo.x)) && near(lo.y, std::round(lo.y)));
    Camera2D moving{{1.234f, 5.678f}, {10, 5}};
    vec2 lo2 = moving.snapped(96).bounds().min * 96.0f;
    CHECK(near(lo2.x, std::round(lo2.x)) && near(lo2.y, std::round(lo2.y)));
}

TEST(camera_view_transform) {
    Camera2D c{{2, 1}, {4, 2}};
    // Full surface: plain world -> clip.
    vec4 v = viewTransform(c, Viewport{}, 800, 400);
    CHECK(near(v.x, 0.25f) && near(v.y, 0.5f) && near(v.z, -0.5f) && near(v.w, -0.5f));
    // Sub-viewport: the camera rect maps onto exactly that pixel rect.
    Viewport vp{100, 50, 400, 200};
    v = viewTransform(c, vp, 800, 400);
    auto toPx = [&](vec2 w) {
        float cx = w.x * v.x + v.z, cy = w.y * v.y + v.w;   // clip, Y up
        return vec2{(cx + 1) * 0.5f * 800, (1 - cy) * 0.5f * 400};
    };
    Rect b = c.bounds();
    CHECK(near(toPx({b.min.x, b.max.y}), {100, 50}));    // top-left
    CHECK(near(toPx({b.max.x, b.min.y}), {500, 250}));   // bottom-right
}

TEST(camera_rotate_scissor) {
    Viewport r{100, 50, 400, 200};   // app space 800x400 (landscape)
    Viewport n0 = rotateToNative(r, 800, 400, 0);
    CHECK(n0 == r);
    Viewport n2 = rotateToNative(r, 800, 400, 2);   // 180: mirrored both ways
    CHECK(n2.x == 300 && n2.y == 150 && n2.w == 400 && n2.h == 200);
    // 90/270: native is 400x800; size swaps and the rect stays inside the image.
    for (int q : {1, 3}) {
        Viewport n = rotateToNative(r, 800, 400, q);
        CHECK(n.w == 200 && n.h == 400);
        CHECK(n.x >= 0 && n.y >= 0 && n.x + int32_t(n.w) <= 400 && n.y + int32_t(n.h) <= 800);
    }
    // 90 then 270 positions differ (opposite sides).
    CHECK(!(rotateToNative(r, 800, 400, 1) == rotateToNative(r, 800, 400, 3)));
    // Full viewport rotates to the full native image.
    Viewport f = rotateToNative(Viewport{}, 800, 400, 1);
    CHECK(f.x == 0 && f.y == 0 && f.w == 400 && f.h == 800);
}
