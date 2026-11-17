#include "mge/render/tilemap.h"
#include <algorithm>
#include <cmath>

namespace mge::render {
namespace {

uint16_t toUnorm16(float v) { return uint16_t(std::lround(clamp(v, 0.0f, 1.0f) * 65535.0f)); }

}  // namespace

uint32_t Tileset::columns() const {
    if (tileW == 0 || textureW < 2 * margin + tileW) return textureW >= tileW && tileW ? 1 : 0;
    return (textureW - 2 * margin + spacing) / (tileW + spacing);
}

uint32_t Tileset::rows() const {
    if (tileH == 0 || textureH < 2 * margin + tileH) return textureH >= tileH && tileH ? 1 : 0;
    return (textureH - 2 * margin + spacing) / (tileH + spacing);
}

vec4 Tileset::uvRect(uint32_t index) const {
    const uint32_t cols = columns() ? columns() : 1;
    const uint32_t col = index % cols, row = index / cols;
    const float x0 = float(margin + col * (tileW + spacing)), y0 = float(margin + row * (tileH + spacing));
    const float in = std::min(inset, std::min(float(tileW), float(tileH)) * 0.5f);
    const float tw = float(textureW ? textureW : 1), th = float(textureH ? textureH : 1);
    return {(x0 + in) / tw, (y0 + in) / th, (x0 + float(tileW) - in) / tw, (y0 + float(tileH) - in) / th};
}

void Tilemap::create(uint32_t w, uint32_t h, const Tileset& ts, vec2 tileSize, uint32_t chunkSize) {
    width_ = w;
    height_ = h;
    chunkSize_ = chunkSize ? chunkSize : 16;
    chunksX_ = (w + chunkSize_ - 1) / chunkSize_;
    chunksY_ = (h + chunkSize_ - 1) / chunkSize_;
    tileSize_ = tileSize;
    tiles_.assign(size_t(w) * h, kEmptyTile);
    chunks_.assign(size_t(chunksX_) * chunksY_, Chunk{});
    setTileset(ts);
}

Tile Tilemap::tile(int32_t x, int32_t y) const {
    if (x < 0 || y < 0 || uint32_t(x) >= width_ || uint32_t(y) >= height_) return kEmptyTile;
    return tiles_[size_t(y) * width_ + uint32_t(x)];
}

void Tilemap::setTile(int32_t x, int32_t y, Tile t) {
    if (x < 0 || y < 0 || uint32_t(x) >= width_ || uint32_t(y) >= height_) return;
    Tile& slot = tiles_[size_t(y) * width_ + uint32_t(x)];
    if (slot == t) return;   // no-op writes don't cost a rebuild
    Chunk& c = chunkAt(uint32_t(x), uint32_t(y));
    const bool wasSet = (slot & kTileIdMask) != 0, isSet = (t & kTileIdMask) != 0;
    if (wasSet != isSet) c.tileCount += isSet ? 1u : uint32_t(-1);
    slot = t;
    c.dirty = true;
}

void Tilemap::fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Tile t) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, int32_t(width_) - 1);
    y1 = std::min(y1, int32_t(height_) - 1);
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x) setTile(x, y, t);
}

void Tilemap::clear() {
    std::fill(tiles_.begin(), tiles_.end(), kEmptyTile);
    for (Chunk& c : chunks_) { c.tileCount = 0; c.dirty = true; }
}

void Tilemap::setTileset(const Tileset& ts) {
    tileset_ = ts;
    uvCache_.resize(ts.count());
    for (uint32_t i = 0; i < uvCache_.size(); ++i) uvCache_[i] = ts.uvRect(i);
    dirtyAll();
}

void Tilemap::setOrigin(vec2 o) {
    origin_ = o;
    dirtyAll();
}

void Tilemap::dirtyAll() {
    for (Chunk& c : chunks_) c.dirty = true;
}

void Tilemap::worldToTile(vec2 world, int32_t& x, int32_t& y) const {
    vec2 l = world - origin_;
    x = int32_t(std::floor(l.x / tileSize_.x));
    y = int32_t(std::floor(l.y / tileSize_.y));
}

Rect Tilemap::tileBounds(int32_t x, int32_t y) const {
    vec2 lo = origin_ + vec2{float(x) * tileSize_.x, float(y) * tileSize_.y};
    return {lo, lo + tileSize_};
}

Rect Tilemap::bounds() const {
    return {origin_, origin_ + vec2{float(width_) * tileSize_.x, float(height_) * tileSize_.y}};
}

void Tilemap::rebuild(uint32_t cx, uint32_t cy) {
    Chunk& c = chunks_[size_t(cy) * chunksX_ + cx];
    c.mesh.clear();
    c.dirty = false;
    if (c.tileCount == 0) { c.mesh.shrink_to_fit(); return; }
    c.mesh.reserve(c.tileCount);
    const uint32_t x0 = cx * chunkSize_, y0 = cy * chunkSize_;
    const uint32_t x1 = std::min(x0 + chunkSize_, width_), y1 = std::min(y0 + chunkSize_, height_);
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const Tile t = tiles_[size_t(y) * width_ + x];
            const uint32_t id = t & kTileIdMask;
            if (id == 0) continue;
            vec4 uv = id - 1 < uvCache_.size() ? uvCache_[id - 1] : vec4{0, 0, 1, 1};
            if (t & kTileFlipX) std::swap(uv.x, uv.z);
            if (t & kTileFlipY) std::swap(uv.y, uv.w);
            SpriteInstance in;
            in.axisX[0] = tileSize_.x; in.axisX[1] = 0;
            in.axisY[0] = 0; in.axisY[1] = tileSize_.y;
            in.origin[0] = origin_.x + float(x) * tileSize_.x;
            in.origin[1] = origin_.y + float(y) * tileSize_.y;
            in.uv[0] = toUnorm16(uv.x); in.uv[1] = toUnorm16(uv.y);
            in.uv[2] = toUnorm16(uv.z); in.uv[3] = toUnorm16(uv.w);
            in.color = color;
            c.mesh.push_back(in);
        }
    }
    ++stats_.chunksRebuilt;
}

void Tilemap::draw(SpriteBatcher& batcher) {
    stats_ = {};
    if (chunks_.empty() || color >> 24 == 0) return;
    if (color != builtColor_) { builtColor_ = color; dirtyAll(); }

    // Camera rect -> inclusive chunk range, clamped to the map.
    const Rect view = batcher.camera().bounds();
    const vec2 chunkWorld = tileSize_ * float(chunkSize_);
    auto range = [](float lo, float hi, float o, float size, uint32_t count, int32_t& a, int32_t& b) {
        a = int32_t(std::floor((lo - o) / size));
        b = int32_t(std::floor((hi - o) / size));
        a = std::max(a, 0);
        b = std::min(b, int32_t(count) - 1);
    };
    int32_t cx0, cx1, cy0, cy1;
    range(view.min.x, view.max.x, origin_.x, chunkWorld.x, chunksX_, cx0, cx1);
    range(view.min.y, view.max.y, origin_.y, chunkWorld.y, chunksY_, cy0, cy1);
    for (int32_t cy = cy0; cy <= cy1; ++cy) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            Chunk& c = chunks_[size_t(cy) * chunksX_ + uint32_t(cx)];
            if (c.tileCount == 0) { if (c.dirty) rebuild(uint32_t(cx), uint32_t(cy)); continue; }
            if (c.dirty) rebuild(uint32_t(cx), uint32_t(cy));
            ++stats_.chunksVisible;
            stats_.tilesDrawn += uint32_t(c.mesh.size());
            batcher.submitInstances(layer, tileset_.texture, c.mesh.data(), uint32_t(c.mesh.size()));
        }
    }
}

uint32_t Tilemap::chunkInstanceCount(uint32_t cx, uint32_t cy) const {
    return cx < chunksX_ && cy < chunksY_ ? uint32_t(chunks_[size_t(cy) * chunksX_ + cx].mesh.size()) : 0;
}

bool Tilemap::chunkDirty(uint32_t cx, uint32_t cy) const {
    return cx < chunksX_ && cy < chunksY_ && chunks_[size_t(cy) * chunksX_ + cx].dirty;
}

}  // namespace mge::render
