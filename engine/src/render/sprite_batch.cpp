#include "mge/render/sprite_batch.h"
#include <algorithm>
#include <cmath>

namespace mge::render {
namespace {

uint32_t toUnorm8(float v) { return uint32_t(std::lround(clamp(v, 0.0f, 1.0f) * 255.0f)); }
uint16_t toUnorm16(float v) { return uint16_t(std::lround(clamp(v, 0.0f, 1.0f) * 65535.0f)); }

}  // namespace

uint32_t packColor(vec4 c) {
    return toUnorm8(c.x) | toUnorm8(c.y) << 8 | toUnorm8(c.z) << 16 | toUnorm8(c.w) << 24;
}

vec4 unpackColor(uint32_t p) {
    constexpr float k = 1.0f / 255.0f;
    return {float(p & 0xFF) * k, float((p >> 8) & 0xFF) * k, float((p >> 16) & 0xFF) * k,
            float(p >> 24) * k};
}

SpriteInstance SpriteBatcher::makeInstance(const Sprite& s) {
    const scene2d::Affine2& m = s.world;
    vec2 ax = m.applyDir({s.size.x, 0});
    vec2 ay = m.applyDir({0, s.size.y});
    vec2 o = m.apply({-s.pivot.x * s.size.x, -s.pivot.y * s.size.y});
    SpriteInstance in;
    in.axisX[0] = ax.x; in.axisX[1] = ax.y;
    in.axisY[0] = ay.x; in.axisY[1] = ay.y;
    in.origin[0] = o.x; in.origin[1] = o.y;
    in.uv[0] = toUnorm16(s.uvRect.x); in.uv[1] = toUnorm16(s.uvRect.y);
    in.uv[2] = toUnorm16(s.uvRect.z); in.uv[3] = toUnorm16(s.uvRect.w);
    in.color = s.color;
    return in;
}

Rect SpriteBatcher::worldBounds(const Sprite& s) {
    SpriteInstance in = makeInstance(s);
    vec2 o{in.origin[0], in.origin[1]}, ax{in.axisX[0], in.axisX[1]}, ay{in.axisY[0], in.axisY[1]};
    vec2 p1 = o + ax, p2 = o + ay, p3 = o + ax + ay;
    return {min(min(o, p1), min(p2, p3)), max(max(o, p1), max(p2, p3))};
}

void SpriteBatcher::begin(const Camera2D& camera) {
    camera_ = camera;
    view_ = camera.bounds();
    pending_.clear();
    keys_.clear();
    instances_.clear();
    batches_.clear();
    stats_ = {};
}

void SpriteBatcher::submit(const Sprite& s) {
    ++stats_.submitted;
    if (s.color >> 24 == 0) { ++stats_.culled; return; }   // fully transparent
    if (cullingEnabled && !worldBounds(s).overlaps(view_)) { ++stats_.culled; return; }
    uint32_t idx = uint32_t(pending_.size());
    pending_.push_back(s);
    keys_.push_back({s.layer, s.texture, idx});
}

void SpriteBatcher::end() {
    // (layer, texture, submission order). Index is unique, so the order is total and stable.
    std::sort(keys_.begin(), keys_.end(), [](const Key& a, const Key& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.texture != b.texture) return a.texture < b.texture;
        return a.index < b.index;
    });
    instances_.reserve(keys_.size());
    for (const Key& k : keys_) {
        const Sprite& s = pending_[k.index];
        if (!batches_.empty() && batches_.back().texture == s.texture && batches_.back().layer == s.layer) {
            ++batches_.back().instanceCount;
        } else {
            batches_.push_back({s.texture, s.layer, uint32_t(instances_.size()), 1});
        }
        instances_.push_back(makeInstance(s));
    }
    stats_.drawn = uint32_t(instances_.size());
    stats_.batches = uint32_t(batches_.size());
}

}  // namespace mge::render
