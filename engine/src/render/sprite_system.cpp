#include "mge/render/sprite_system.h"

namespace mge::render {

void registerRender2D(serial::Registry& reg) {
    if (reg.find("SpriteRenderer")) return;
    reg.add<SpriteRenderer>("SpriteRenderer", {MGE_FIELD(SpriteRenderer, size, Vec2),
                                               MGE_FIELD(SpriteRenderer, pivot, Vec2),
                                               MGE_FIELD(SpriteRenderer, uvRect, Vec4),
                                               MGE_FIELD_HINT(SpriteRenderer, color, U32, Color),
                                               MGE_FIELD(SpriteRenderer, layer, I16),
                                               MGE_FIELD_HINT(SpriteRenderer, visible, U16, Bool)});
}

uint32_t submitSprites(ecs::World& world, SpriteBatcher& batcher) {
    uint32_t n = 0;
    world.eachChunk<scene2d::WorldTransform2D, SpriteRenderer>(
        [&](uint32_t count, const ecs::Entity*, scene2d::WorldTransform2D* xf, SpriteRenderer* sr) {
            for (uint32_t i = 0; i < count; ++i) {
                if (!sr[i].visible) continue;
                Sprite s;
                s.world = xf[i].m;
                s.size = sr[i].size;
                s.pivot = sr[i].pivot;
                s.uvRect = sr[i].uvRect;
                s.color = sr[i].color;
                s.texture = sr[i].texture;
                s.layer = sr[i].layer;
                batcher.submit(s);
                ++n;
            }
        });
    return n;
}

}  // namespace mge::render
