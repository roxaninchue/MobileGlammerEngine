#pragma once
// ECS sprites (M1 #20): a SpriteRenderer component drawn at the entity's WorldTransform2D.
// submitSprites() walks every entity with both, column by column, and feeds the SpriteBatcher
// (which culls, sorts and batches). Run TransformSystem2D::update first.
#include <cstdint>
#include "mge/ecs.h"
#include "mge/render/sprite_batch.h"
#include "mge/transform2d.h"

namespace mge::render {

struct SpriteRenderer {
    vec2 size{1, 1};
    vec2 pivot{0.5f, 0.5f};
    vec4 uvRect{0, 0, 1, 1};
    uint32_t color = 0xFFFFFFFFu;
    TextureId texture = kWhiteTexture;
    int16_t layer = 0;
    uint16_t visible = 1;
};

// Returns the number of sprites submitted (before culling).
uint32_t submitSprites(ecs::World& world, SpriteBatcher& batcher);

}  // namespace mge::render
