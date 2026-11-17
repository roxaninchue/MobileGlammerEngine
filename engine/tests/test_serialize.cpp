#include "test_main.h"
#include "mge/serialize.h"
#include "mge/transform2d.h"
#include <cmath>
using namespace mge;
using namespace mge::scene2d;
using ecs::Entity;

namespace {
bool close2(vec2 a, vec2 b) { return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f; }

void buildScene(ecs::World& w, TransformSystem2D& ts, Entity e[3]) {
    for (int i = 0; i < 3; ++i) e[i] = w.create();
    ts.attach(w, e[0], {100, 0}, radians(90));
    ts.attach(w, e[1], {10, 0});
    ts.attach(w, e[2], {5, 5}, 0.25f, {2, 3});
    ts.setParent(w, e[1], e[0]);
    ts.update(w);
}

struct HealthV1 { int32_t hp; };
struct HealthV2 { float regen; int32_t hp; int32_t maxHp = 100; };
struct Junk { uint32_t v; };
}  // namespace

TEST(serialize_binary_roundtrip) {
    serial::Registry reg;
    serial::registerScene2D(reg);
    ecs::World a;
    TransformSystem2D ta;
    Entity e[3];
    buildScene(a, ta, e);
    auto bin = serial::saveBinary(a, reg);
    CHECK(bin.size() < 256);

    ecs::World b;
    TransformSystem2D tb;
    std::vector<Entity> made;
    CHECK(serial::loadBinary(b, reg, bin.data(), bin.size(), &made));
    CHECK(made.size() == 3 && b.aliveCount() == 3);
    tb.update(b);
    for (int i = 0; i < 3; ++i)
        CHECK(close2(b.get<WorldTransform2D>(made[i])->m.translation(), a.get<WorldTransform2D>(e[i])->m.translation()));
    CHECK(tb.parentOf(b, made[1]) == made[0]);
    CHECK(close2(b.get<Transform2D>(made[2])->scale, {2, 3}));
}

TEST(serialize_json_roundtrip) {
    serial::Registry reg;
    serial::registerScene2D(reg);
    ecs::World a;
    TransformSystem2D ta;
    Entity e[3];
    buildScene(a, ta, e);
    std::string js = serial::saveJson(a, reg);
    CHECK(js.find("\"Transform2D\"") != std::string::npos && js.find("\"Parent\":{\"entity\":0}") != std::string::npos);

    ecs::World b;
    TransformSystem2D tb;
    std::vector<Entity> made;
    CHECK(serial::loadJson(b, reg, js, &made));
    tb.update(b);
    for (int i = 0; i < 3; ++i)
        CHECK(close2(b.get<WorldTransform2D>(made[i])->m.translation(), a.get<WorldTransform2D>(e[i])->m.translation()));
    CHECK(serial::saveJson(b, reg) == js);   // stable output
}

TEST(serialize_json_ai_edit) {
    // What an AI edit looks like: arbitrary ids, partial fields, an unknown component.
    serial::Registry reg;
    serial::registerScene2D(reg);
    const char* js = R"({"entities":[
        {"id":7, "Transform2D":{"position":[50,0]}},
        {"id":9, "Transform2D":{"position":[0,10]}, "Parent":{"entity":7}, "Sparkle":{"x":1}}
    ]})";
    ecs::World w;
    TransformSystem2D ts;
    std::vector<Entity> made;
    CHECK(serial::loadJson(w, reg, js, &made));
    ts.update(w);
    CHECK(made.size() == 2);
    CHECK(close2(w.get<WorldTransform2D>(made[1])->m.translation(), {50, 10}));
    CHECK(close2(w.get<Transform2D>(made[0])->scale, {1, 1}));   // defaults kept
}

TEST(serialize_schema_evolution) {
    serial::Registry r1;
    r1.add<HealthV1>("Health", {MGE_FIELD(HealthV1, hp, I32)});
    r1.add<Junk>("Junk", {MGE_FIELD(Junk, v, U32)});
    ecs::World a;
    Entity e = a.create();
    a.add<HealthV1>(e, {42});
    a.add<Junk>(e, {7});
    auto bin = serial::saveBinary(a, r1);

    serial::Registry r2;   // Health gained fields and reordered; Junk was deleted
    r2.add<HealthV2>("Health", {MGE_FIELD(HealthV2, regen, F32), MGE_FIELD(HealthV2, hp, I32), MGE_FIELD(HealthV2, maxHp, I32)});
    ecs::World b;
    std::vector<Entity> made;
    CHECK(serial::loadBinary(b, r2, bin.data(), bin.size(), &made));
    auto* h = b.get<HealthV2>(made[0]);
    CHECK(h && h->hp == 42 && h->maxHp == 100 && h->regen == 0);
    CHECK(!b.has<Junk>(made[0]));
}

TEST(serialize_rejects_corrupt) {
    serial::Registry reg;
    serial::registerScene2D(reg);
    ecs::World a;
    TransformSystem2D ta;
    Entity e[3];
    buildScene(a, ta, e);
    auto bin = serial::saveBinary(a, reg);

    ecs::World b;
    for (size_t cut : {size_t(0), size_t(3), bin.size() / 2, bin.size() - 1})
        CHECK(!serial::loadBinary(b, reg, bin.data(), cut));
    auto bad = bin;
    bad[0] = 'X';
    CHECK(!serial::loadBinary(b, reg, bad.data(), bad.size()));
    CHECK(!serial::loadJson(b, reg, R"({"entities":[{"Transform2D":{"position":"oops"}}]})"));
    CHECK(!serial::loadJson(b, reg, R"({"entities":[)"));
    CHECK(b.aliveCount() == 0);   // failed loads leave nothing behind
}
