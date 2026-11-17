#include "mge/render/camera.h"
#include <algorithm>
#include <cmath>

namespace mge::render {

Camera2D Camera2D::fitHeight(vec2 center, float visibleHeight, uint32_t w, uint32_t h) {
    float aspect = h ? float(w) / float(h) : 1.0f;
    Camera2D c;
    c.center = center;
    c.halfExtent = {visibleHeight * 0.5f * aspect, visibleHeight * 0.5f};
    return c;
}

Camera2D Camera2D::snapped(float ppu) const {
    if (!(ppu > 0)) return *this;
    Camera2D c = *this;
    vec2 lo = center - halfExtent;
    c.center.x += std::round(lo.x * ppu) / ppu - lo.x;
    c.center.y += std::round(lo.y * ppu) / ppu - lo.y;
    return c;
}

vec2 Camera2D::viewportToWorld(vec2 px, uint32_t w, uint32_t h) const {
    float nx = w ? px.x / float(w) * 2.0f - 1.0f : 0.0f;
    float ny = h ? 1.0f - px.y / float(h) * 2.0f : 0.0f;
    return center + vec2{nx, ny} * halfExtent;
}

vec2 Camera2D::worldToViewport(vec2 world, uint32_t w, uint32_t h) const {
    vec2 n = (world - center) * scale();
    return {(n.x + 1.0f) * 0.5f * float(w), (1.0f - n.y) * 0.5f * float(h)};
}

ScreenLayout ScreenLayout::compute(const DesignSpec& d, uint32_t W, uint32_t H, const Insets& in) {
    ScreenLayout L;
    L.surfaceW = W;
    L.surfaceH = H;
    if (W == 0 || H == 0) {   // no surface yet: keep the design area so callers stay sane
        L.camera = {d.area.center(), d.area.size() * 0.5f};
        L.safeWorld = d.area;
        return L;
    }
    // Safe rect in pixels; insets that eat the whole surface are ignored on that axis.
    float sx0 = float(in.left), sy0 = float(in.top);
    float sw = float(W) - float(in.left) - float(in.right);
    float sh = float(H) - float(in.top) - float(in.bottom);
    if (sw < 1) { sx0 = 0; sw = float(W); }
    if (sh < 1) { sy0 = 0; sh = float(H); }

    vec2 ds = d.area.size();
    if (ds.x <= 0) ds.x = 1;
    if (ds.y <= 0) ds.y = 1;

    float s = 1;
    switch (d.mode) {
        case ScaleMode::Letterbox:
        case ScaleMode::Expand: s = std::min(sw / ds.x, sh / ds.y); break;
        case ScaleMode::Crop: s = std::max(float(W) / ds.x, float(H) / ds.y); break;
        case ScaleMode::FitWidth: s = sw / ds.x; break;
        case ScaleMode::FitHeight: s = sh / ds.y; break;
    }
    if (d.pixelPerfect && d.artPixelsPerUnit > 0) {
        float k = s / d.artPixelsPerUnit;
        // Crop must still cover the screen, everything else must still fit.
        k = d.mode == ScaleMode::Crop ? std::ceil(k - 1e-4f) : std::floor(k + 1e-4f);
        if (k < 1) k = 1;
        L.pixelScale = uint32_t(k);
        s = k * d.artPixelsPerUnit;
    }
    L.pixelsPerUnit = s;

    if (d.mode == ScaleMode::Letterbox) {
        uint32_t vw = uint32_t(std::min(float(W), std::round(ds.x * s)));
        uint32_t vh = uint32_t(std::min(float(H), std::round(ds.y * s)));
        if (vw == 0) vw = 1;
        if (vh == 0) vh = 1;
        L.viewport.w = vw;
        L.viewport.h = vh;
        L.viewport.x = int32_t(std::floor(sx0 + (sw - float(vw)) * 0.5f));
        L.viewport.y = int32_t(std::floor(sy0 + (sh - float(vh)) * 0.5f));
        L.viewport.x = std::clamp(L.viewport.x, 0, int32_t(W - vw));
        L.viewport.y = std::clamp(L.viewport.y, 0, int32_t(H - vh));
        L.camera.center = d.area.center();
        L.camera.halfExtent = vec2{float(vw), float(vh)} * (0.5f / s);
    } else {
        L.viewport = {0, 0, W, H};
        // Pin the design center to the safe rect's center (screen center for Crop).
        vec2 anchor = d.mode == ScaleMode::Crop ? vec2{float(W) * 0.5f, float(H) * 0.5f}
                                                : vec2{sx0 + sw * 0.5f, sy0 + sh * 0.5f};
        vec2 off{(anchor.x - float(W) * 0.5f) / s, -(anchor.y - float(H) * 0.5f) / s};
        L.camera.center = d.area.center() - off;
        L.camera.halfExtent = vec2{float(W), float(H)} * (0.5f / s);
    }
    if (d.pixelPerfect) L.camera = L.camera.snapped(s);

    // Safe area (clipped to the viewport) in world space.
    float x0 = std::max(sx0, float(L.viewport.x)), y0 = std::max(sy0, float(L.viewport.y));
    float x1 = std::min(sx0 + sw, float(L.viewport.x) + float(L.viewport.w));
    float y1 = std::min(sy0 + sh, float(L.viewport.y) + float(L.viewport.h));
    vec2 a = L.screenToWorld({x0, y1}), b = L.screenToWorld({x1, y0});
    L.safeWorld = {min(a, b), max(a, b)};
    return L;
}

vec2 ScreenLayout::screenToWorld(vec2 px) const {
    uint32_t w = viewport.full() ? surfaceW : viewport.w;
    uint32_t h = viewport.full() ? surfaceH : viewport.h;
    return camera.viewportToWorld({px.x - float(viewport.x), px.y - float(viewport.y)}, w, h);
}

vec2 ScreenLayout::worldToScreen(vec2 world) const {
    uint32_t w = viewport.full() ? surfaceW : viewport.w;
    uint32_t h = viewport.full() ? surfaceH : viewport.h;
    vec2 p = camera.worldToViewport(world, w, h);
    return {p.x + float(viewport.x), p.y + float(viewport.y)};
}

bool ScreenLayout::screenInViewport(vec2 px) const {
    if (viewport.full()) return px.x >= 0 && px.y >= 0 && px.x < float(surfaceW) && px.y < float(surfaceH);
    return px.x >= float(viewport.x) && px.y >= float(viewport.y) &&
           px.x < float(viewport.x) + float(viewport.w) && px.y < float(viewport.y) + float(viewport.h);
}

vec4 viewTransform(const Camera2D& cam, const Viewport& vp, uint32_t W, uint32_t H) {
    vec2 s = cam.scale();
    vec2 o = -cam.center * s;
    if (vp.full() || W == 0 || H == 0) return {s.x, s.y, o.x, o.y};
    float kx = float(vp.w) / float(W), ky = float(vp.h) / float(H);
    float bx = (2.0f * float(vp.x) + float(vp.w)) / float(W) - 1.0f;
    float by = 1.0f - (2.0f * float(vp.y) + float(vp.h)) / float(H);
    return {s.x * kx, s.y * ky, o.x * kx + bx, o.y * ky + by};
}

Viewport rotateToNative(const Viewport& r, uint32_t AW, uint32_t AH, int turns) {
    turns &= 3;
    Viewport in = r.full() ? Viewport{0, 0, AW, AH} : r;
    if (turns == 0 || AW == 0 || AH == 0) return in;
    const bool odd = turns & 1;
    const float NW = float(odd ? AH : AW), NH = float(odd ? AW : AH);
    static const float kC[4] = {1, 0, -1, 0}, kS[4] = {0, 1, 0, -1};
    const float c = kC[turns], s = kS[turns];
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    const float xs[2] = {float(in.x), float(in.x) + float(in.w)};
    const float ys[2] = {float(in.y), float(in.y) + float(in.h)};
    for (float px : xs) {
        for (float py : ys) {
            // App pixels -> Vulkan clip (Y down, as after the shader's flip) -> rotate -> native pixels.
            float nx = 2.0f * px / float(AW) - 1.0f, ny = 2.0f * py / float(AH) - 1.0f;
            float rx = c * nx - s * ny, ry = s * nx + c * ny;
            float qx = (rx + 1.0f) * 0.5f * NW, qy = (ry + 1.0f) * 0.5f * NH;
            lo[0] = std::min(lo[0], qx); hi[0] = std::max(hi[0], qx);
            lo[1] = std::min(lo[1], qy); hi[1] = std::max(hi[1], qy);
        }
    }
    Viewport out;
    out.x = int32_t(std::lround(lo[0]));
    out.y = int32_t(std::lround(lo[1]));
    out.w = uint32_t(std::lround(hi[0]) - out.x);
    out.h = uint32_t(std::lround(hi[1]) - out.y);
    return out;
}

}  // namespace mge::render
