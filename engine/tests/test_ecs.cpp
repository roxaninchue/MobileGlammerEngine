#include "test_main.h"
#include "mge/ecs.h"
using namespace mge::ecs;

namespace {
struct Pos { float x, y; };
struct Vel { float x, y; };
struct Tag { int v; };
}

TEST(ecs_create_destroy_generations) {
    World w;
    Entity a = w.create(), b = w.create();
    CHECK(w.alive(a) && w.alive(b) && w.aliveCount() == 2);
    w.destroy(a);
    CHECK(!w.alive(a) && w.aliveCount() == 1);
    Entity c = w.create();               // reuses a's slot with a new generation
    CHECK(c.index == a.index && c.generation != a.generation);
    CHECK(!w.alive(a) && w.alive(c));
    CHECK(w.get<Pos>(a) == nullptr);     // stale handle is safe
}

TEST(ecs_add_get_remove) {
    World w;
    Entity e = w.create();
    w.add<Pos>(e, {1, 2});
    w.add<Vel>(e, {3, 4});
    CHECK(w.has<Pos>(e) && w.has<Vel>(e) && !w.has<Tag>(e));
    CHECK(w.get<Pos>(e)->x == 1 && w.get<Vel>(e)->y == 4);   // data survives archetype moves
    w.remove<Pos>(e);
    CHECK(!w.has<Pos>(e) && w.get<Vel>(e)->x == 3);
    w.add<Pos>(e).x = 9;                 // default-initialised then edited in place
    CHECK(w.get<Pos>(e)->x == 9 && w.get<Pos>(e)->y == 0);
}

TEST(ecs_swap_remove_keeps_others) {
    World w;
    Entity e[5];
    for (int i = 0; i < 5; ++i) { e[i] = w.create(); w.add<Tag>(e[i], {i}); }
    w.destroy(e[1]);
    w.remove<Tag>(e[3]);
    CHECK(w.get<Tag>(e[0])->v == 0 && w.get<Tag>(e[2])->v == 2 && w.get<Tag>(e[4])->v == 4);
    CHECK(w.count<Tag>() == 3);
}

TEST(ecs_queries) {
    World w;
    for (int i = 0; i < 1000; ++i) {
        Entity e = w.create();
        w.add<Pos>(e, {float(i), 0});
        if (i % 2 == 0) w.add<Vel>(e, {1, 2});
        if (i % 10 == 0) w.add<Tag>(e, {i});
    }
    CHECK(w.count<Pos>() == 1000 && (w.count<Pos, Vel>()) == 500 && w.count<Tag>() == 100);
    w.each<Pos, Vel>([](Entity, Pos& p, Vel& v) { p.x += v.x; p.y += v.y; });
    int moved = 0;
    w.each<Pos>([&](Entity, Pos& p) { if (p.y == 2) ++moved; });
    CHECK(moved == 500);
    uint32_t seen = 0;
    w.eachChunk<Vel>([&](uint32_t n, const Entity*, Vel*) { seen += n; });
    CHECK(seen == 500);
    CHECK(w.archetypeCount() <= 5);
}

TEST(ecs_scale_100k) {
    World w;
    for (int i = 0; i < 100000; ++i) { Entity e = w.create(); w.add<Pos>(e); w.add<Vel>(e, {1, 1}); }
    for (int f = 0; f < 10; ++f) w.each<Pos, Vel>([](Entity, Pos& p, Vel& v) { p.x += v.x; });
    float sum = 0;
    w.each<Pos>([&](Entity, Pos& p) { sum += p.x; });
    CHECK(sum == 1000000.f);
    CHECK(w.memoryBytes() < 8 * 1024 * 1024);   // 24 B/entity of data + records, with doubling slack
}
