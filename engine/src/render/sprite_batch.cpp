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
    prebuilt_.clear();
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
    keys_.push_back({s.layer, s.texture, uint32_t(keys_.size()), idx});
}

void SpriteBatcher::submitInstances(int16_t layer, TextureId texture, const SpriteInstance* data, uint32_t count) {
    if (!data || count == 0) return;
    stats_.submitted += count;
    uint32_t idx = uint32_t(prebuilt_.size());
    prebuilt_.push_back({data, count});
    keys_.push_back({layer, texture, uint32_t(keys_.size()), idx | kPrebuilt});
}

void SpriteBatcher::end() {
    // (layer, texture, submission order). Order is unique, so the sort is total and stable.
    std::sort(keys_.begin(), keys_.end(), [](const Key& a, const Key& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.texture != b.texture) return a.texture < b.texture;
        return a.order < b.order;
    });
    size_t total = pending_.size();
    for (const Prebuilt& p : prebuilt_) total += p.count;
    instances_.reserve(total);
    for (const Key& k : keys_) {
        const uint32_t n = k.ref & kPrebuilt ? prebuilt_[k.ref & ~kPrebuilt].count : 1;
        if (!batches_.empty() && batches_.back().texture == k.texture && batches_.back().layer == k.layer) {
            batches_.back().instanceCount += n;
        } else {
            batches_.push_back({k.texture, k.layer, uint32_t(instances_.size()), n});
        }
        if (k.ref & kPrebuilt) {
            const Prebuilt& p = prebuilt_[k.ref & ~kPrebuilt];
            instances_.insert(instances_.end(), p.data, p.data + p.count);
        } else {
            instances_.push_back(makeInstance(pending_[k.ref]));
        }
    }
    stats_.drawn = uint32_t(instances_.size());
    stats_.batches = uint32_t(batches_.size());
}

}  // namespace mge::render
