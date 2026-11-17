#pragma once
// Chunked tilemaps (M1 #16).
//  - The map is split into square chunks (16x16 tiles by default). Each chunk caches its
//    SpriteInstance array (its "static mesh") and only rebuilds it after setTile() touches it.
//  - draw() works out which chunks the camera can see, from the camera rect alone (no per-tile
//    test), and hands their cached instances to the SpriteBatcher in one go per chunk. Nothing
//    off screen is touched, so cost scales with the screen, not the map.
//  - Tiles use the sprite pipeline: same 36-byte instance, same shaders, one batch per
//    (layer, tileset texture). Tile (0,0) sits at `origin`; x grows right, y grows up.
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <vector>
#include "mge/math.h"
#include "mge/render/camera.h"
#include "mge/render/sprite_batch.h"

namespace mge::render {

// Tile value: low 14 bits are the tileset index + 1 (0 = empty), top bits are flips.
using Tile = uint16_t;
constexpr Tile kEmptyTile = 0;
constexpr Tile kTileFlipX = 0x8000;
constexpr Tile kTileFlipY = 0x4000;
constexpr Tile kTileIdMask = 0x3FFF;
constexpr Tile makeTile(uint16_t tilesetIndex, bool flipX = false, bool flipY = false) {
    return Tile(((tilesetIndex + 1) & kTileIdMask) | (flipX ? kTileFlipX : 0) | (flipY ? kTileFlipY : 0));
}

// Grid atlas: tiles of tileW x tileH texels, `margin` texels around the sheet and `spacing`
// between tiles (Tiled's conventions). Index 0 is the top-left tile, row-major.
struct Tileset {
    TextureId texture = kWhiteTexture;
    uint32_t textureW = 1, textureH = 1;
    uint32_t tileW = 1, tileH = 1;
    uint32_t margin = 0, spacing = 0;
    // UVs are pulled in by this many texels on every side so linear filtering never samples a
    // neighbouring tile (the usual cause of seams). 0 for nearest-filtered pixel art with padding.
    float inset = 0.5f;

    uint32_t columns() const;
    uint32_t rows() const;
    uint32_t count() const { return columns() * rows(); }
    vec4 uvRect(uint32_t index) const;   // u0, v0 (top-left), u1, v1 (bottom-right)
};

struct TilemapStats {
    uint32_t chunksVisible = 0;   // chunks inside the camera rect that hold tiles
    uint32_t chunksRebuilt = 0;   // meshes rebuilt during this draw()
    uint32_t tilesDrawn = 0;
};

class Tilemap {
public:
    // widthTiles x heightTiles map, all empty.
    void create(uint32_t widthTiles, uint32_t heightTiles, const Tileset& tileset, vec2 tileSize = {1, 1},
                uint32_t chunkSize = 16);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t chunkSize() const { return chunkSize_; }
    uint32_t chunksX() const { return chunksX_; }
    uint32_t chunksY() const { return chunksY_; }
    const Tileset& tileset() const { return tileset_; }
    vec2 tileSize() const { return tileSize_; }

    // Out-of-range coordinates read as empty and ignore writes.
    Tile tile(int32_t x, int32_t y) const;
    void setTile(int32_t x, int32_t y, Tile t);
    void fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Tile t);   // inclusive rect, clipped
    void clear();

    // Changing the tileset or placement re-dirties every chunk.
    void setTileset(const Tileset& ts);
    void setOrigin(vec2 o);
    vec2 origin() const { return origin_; }

    int16_t layer = -100;           // tilemaps usually sit behind sprites
    uint32_t color = 0xFFFFFFFFu;   // tint for every tile; changing it re-dirties every chunk

    // World <-> tile coordinates (floor; may be outside the map).
    void worldToTile(vec2 world, int32_t& x, int32_t& y) const;
    Rect tileBounds(int32_t x, int32_t y) const;
    Rect bounds() const;

    // Rebuilds dirty visible chunks, then submits every visible chunk's mesh.
    // Call between batcher.begin() and batcher.end(); culls against batcher.camera().
    void draw(SpriteBatcher& batcher);
    const TilemapStats& stats() const { return stats_; }

    // Exposed for tests: chunk mesh size and whether it is waiting for a rebuild.
    uint32_t chunkInstanceCount(uint32_t cx, uint32_t cy) const;
    bool chunkDirty(uint32_t cx, uint32_t cy) const;

private:
    struct Chunk {
        std::vector<SpriteInstance> mesh;
        uint32_t tileCount = 0;   // non-empty tiles, kept by setTile so empty chunks skip instantly
        bool dirty = true;
    };
    Chunk& chunkAt(uint32_t tx, uint32_t ty) { return chunks_[(ty / chunkSize_) * chunksX_ + tx / chunkSize_]; }
    void rebuild(uint32_t cx, uint32_t cy);
    void dirtyAll();

    uint32_t width_ = 0, height_ = 0, chunkSize_ = 16, chunksX_ = 0, chunksY_ = 0;
    Tileset tileset_;
    vec2 tileSize_{1, 1};
    vec2 origin_{};
    uint32_t builtColor_ = 0xFFFFFFFFu;
    std::vector<Tile> tiles_;
    std::vector<Chunk> chunks_;
    std::vector<vec4> uvCache_;   // per tileset index
    TilemapStats stats_;
};

}  // namespace mge::render
