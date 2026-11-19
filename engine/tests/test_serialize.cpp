#include "test_main.h"
#include "mge/serialize.h"
#include "mge/transform2d.h"
#include <cmath>
#include <cstring>
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

namespace {
struct Wide { vec4 tint{1, 2, 3, 4}; int16_t layer = -3; uint16_t flags = 7; Name label; };
}

TEST(serialize_v2_field_types_roundtrip) {
    serial::Registry reg;
    serial::registerScene2D(reg);
    reg.add<Wide>("Wide", {MGE_FIELD(Wide, tint, Vec4), MGE_FIELD(Wide, layer, I16), MGE_FIELD(Wide, flags, U16),
                           MGE_FIELD(Wide, label, Str32)});
    ecs::World a;
    Entity e = a.create();
    Wide w;
    w.tint = {0.5f, -1, 2, 8};
    w.layer = -300;
    w.flags = 65000;
    w.label = Name::of("Crate \"A\"\\\n\xC3\xA9");   // quote, backslash, newline, e-acute
    a.add<Wide>(e, w);
    a.add<Name>(e, Name::of("Player"));

    for (int pass = 0; pass < 2; ++pass) {
        ecs::World b;
        std::vector<Entity> made;
        if (pass == 0) {
            auto bin = serial::saveBinary(a, reg);
            CHECK(serial::loadBinary(b, reg, bin.data(), bin.size(), &made));
        } else {
            std::string js = serial::saveJson(a, reg);
            CHECK(js.find("\"version\":2") != std::string::npos);
            CHECK(js.find("\"label\":\"Crate \\\"A\\\"\\\\\\n\xC3\xA9\"") != std::string::npos);
            CHECK(serial::loadJson(b, reg, js, &made));
        }
        CHECK(made.size() == 1);
        const Wide* r = b.get<Wide>(made[0]);
        CHECK(r && r->tint.x == 0.5f && r->tint.w == 8 && r->layer == -300 && r->flags == 65000);
        CHECK(r && r->label.view() == w.label.view());
        CHECK(b.get<Name>(made[0]) && b.get<Name>(made[0])->view() == "Player");
    }
}

TEST(serialize_name_limits_and_unicode_escapes) {
    // 40 bytes of text: cut to 31 without splitting the 2-byte character at the boundary.
    std::string s(30, 'x');
    s += "\xC3\xA9tail";
    Name n = Name::of(s);
    CHECK(n.view().size() == 30);
    serial::Registry reg;
    serial::registerScene2D(reg);
    ecs::World w;
    std::vector<Entity> made;
    CHECK(serial::loadJson(w, reg, R"({"entities":[{"Name":{"text":"caf\u00e9 \u4e2d"}}]})", &made));
    CHECK(w.get<Name>(made[0])->view() == "caf\xC3\xA9 \xE4\xB8\xAD");
    CHECK(!serial::loadJson(w, reg, R"({"entities":[{"Name":{"text":5}}]})"));
    CHECK(!serial::loadJson(w, reg, R"({"entities":[{"Name":{"text":"\u12G4"}}]})"));
}

TEST(serialize_loads_v1_binary) {
    // A v1 file (Transform2D only, as M1 wrote it) still loads with v2 code.
    serial::Registry reg;
    serial::registerScene2D(reg);
    std::vector<uint8_t> v1;
    auto put = [&](const void* p, size_t n) { auto* c = static_cast<const uint8_t*>(p); v1.insert(v1.end(), c, c + n); };
    auto u16 = [&](uint16_t v) { put(&v, 2); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto str = [&](const char* s) { u16(uint16_t(std::strlen(s))); put(s, std::strlen(s)); };
    put("MGES", 4); u32(1); u32(1);
    str("Transform2D"); u16(2);
    str("position"); v1.push_back(uint8_t(serial::FieldType::Vec2));
    str("rotation"); v1.push_back(uint8_t(serial::FieldType::F32));
    u32(1); u16(1); u16(0);
    float pos[2] = {3, 4}, rot = 0.5f;
    put(pos, 8); put(&rot, 4);
    ecs::World w;
    std::vector<Entity> made;
    CHECK(serial::loadBinary(w, reg, v1.data(), v1.size(), &made));
    CHECK(made.size() == 1 && close2(w.get<Transform2D>(made[0])->position, {3, 4}));
    v1[4] = 3;   // a future version is refused
    ecs::World w2;
    CHECK(!serial::loadBinary(w2, reg, v1.data(), v1.size()));
}
