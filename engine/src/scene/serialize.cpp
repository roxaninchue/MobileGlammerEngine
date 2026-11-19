#include "mge/serialize.h"
#include "mge/log.h"
#include "mge/transform2d.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace mge::serial {
using ecs::Entity;
using ecs::World;

namespace {


// On disk an entity reference is a 4-byte slot; every other type is stored as in memory.
uint32_t diskSize(FieldType t) { return t == FieldType::Entity ? 4 : fieldSize(t); }

std::vector<Entity> collect(World& w) {
    std::vector<Entity> v;
    v.reserve(w.aliveCount());
    w.eachEntity([&](Entity e) { v.push_back(e); });
    std::sort(v.begin(), v.end(), [](Entity a, Entity b) { return a.index < b.index; });   // deterministic
    return v;
}

struct Remap {
    std::unordered_map<uint32_t, uint32_t> slot;
    explicit Remap(const std::vector<Entity>& ents) {
        for (uint32_t i = 0; i < ents.size(); ++i) slot[ents[i].index] = i;
    }
    uint32_t operator()(World& w, Entity e) const {
        if (!w.alive(e)) return UINT32_MAX;
        auto it = slot.find(e.index);
        return it == slot.end() ? UINT32_MAX : it->second;
    }
};

void runHooks(World& w, const Registry& reg, const std::vector<Entity>& made) {
    for (Entity e : made)
        for (const auto& t : reg.types())
            if (t.onAdd && w.getRaw(e, t.id)) t.onAdd(w, e);
}

bool rollback(World& w, std::vector<Entity>& made) {
    for (Entity e : made) w.destroy(e);
    made.clear();
    return false;
}

// ------------------------------------------------------------------ binary
struct Writer {
    std::vector<uint8_t> b;
    void put(const void* p, size_t n) { auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void str(std::string_view s) { u16(uint16_t(s.size())); put(s.data(), s.size()); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool get(void* d, size_t n) {
        if (!ok || size_t(end - p) < n) { ok = false; return false; }
        std::memcpy(d, p, n);
        p += n;
        return true;
    }
    uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; get(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; get(&v, 4); return v; }
    std::string str() {
        uint16_t n = u16();
        if (!ok || size_t(end - p) < n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    size_t remaining() const { return size_t(end - p); }
};

// ------------------------------------------------------------------ JSON
// Members are a named struct rather than std::pair<std::string, JVal>: pair needs JVal complete,
// which newer libstdc++ enforces (clang 19 + GCC 14 headers reject the recursive pair).
struct JMember;
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } k = Null;
    double n = 0;
    bool b = false;
    std::string s;
    std::vector<JVal> a;
    std::vector<JMember> o;
    const JVal* get(std::string_view key) const;
};
struct JMember {
    std::string first;
    JVal second;
};
inline const JVal* JVal::get(std::string_view key) const {
    for (auto& kv : o) if (kv.first == key) return &kv.second;
    return nullptr;
}

struct Parser {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if (size_t(e - p) >= n && std::memcmp(p, s, n) == 0) { p += n; return true; }
        return false;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p >= e) return false;
            char x = *p++;
            switch (x) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // \uXXXX -> UTF-8 (BMP; surrogate halves become U+FFFD).
                    if (e - p < 4) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = p[i];
                        unsigned d = h >= '0' && h <= '9' ? unsigned(h - '0')
                                   : h >= 'a' && h <= 'f' ? unsigned(h - 'a' + 10)
                                   : h >= 'A' && h <= 'F' ? unsigned(h - 'A' + 10) : 16u;
                        if (d > 15) return false;
                        cp = cp * 16 + d;
                    }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else {
                        out += char(0xE0 | (cp >> 12));
                        out += char(0x80 | ((cp >> 6) & 0x3F));
                        out += char(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += x;
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool val(JVal& v) {
        if (++depth > 64) return false;
        ws();
        bool r = inner(v);
        --depth;
        return r;
    }
    bool inner(JVal& v) {
        if (p >= e) return false;
        char c = *p;
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            char close = obj ? '}' : ']';
            v.k = obj ? JVal::Obj : JVal::Arr;
            ++p;
            ws();
            if (p < e && *p == close) { ++p; return true; }
            for (;;) {
                ws();
                JVal x;
                if (obj) {
                    std::string key;
                    if (!str(key)) return false;
                    ws();
                    if (p >= e || *p != ':') return false;
                    ++p;
                    if (!val(x)) return false;
                    v.o.push_back(JMember{std::move(key), std::move(x)});
                } else {
                    if (!val(x)) return false;
                    v.a.push_back(std::move(x));
                }
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == close) { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.k = JVal::Str; return str(v.s); }
        if (lit("true")) { v.k = JVal::Bool; v.b = true; return true; }
        if (lit("false")) { v.k = JVal::Bool; return true; }
        if (lit("null")) { v.k = JVal::Null; return true; }
        char* end = nullptr;
        v.n = std::strtod(p, &end);   // buffer is NUL-terminated (owned std::string)
        if (end == p) return false;
        v.k = JVal::Num;
        p = end;
        return true;
    }
};

void appendStr(std::string& out, std::string_view v) {
    out += '"';
    for (char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void appendNum(std::string& out, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    out += buf;
}

}  // namespace

uint32_t fieldSize(FieldType t) {
    switch (t) {
        case FieldType::Vec2: case FieldType::Entity: return 8;
        case FieldType::Vec4: return 16;
        case FieldType::I16: case FieldType::U16: return 2;
        case FieldType::Str32: return 32;
        default: return 4;
    }
}

const Registry::Type* Registry::find(std::string_view name) const {
    for (const auto& t : types_) if (t.name == name) return &t;
    return nullptr;
}

void registerScene2D(Registry& reg) {
    using namespace scene2d;
    auto& t = reg.add<Transform2D>("Transform2D", {MGE_FIELD(Transform2D, position, Vec2),
                                                    MGE_FIELD_HINT(Transform2D, rotation, F32, Angle),
                                                    MGE_FIELD(Transform2D, scale, Vec2)});
    t.onAdd = [](World& w, Entity e) {
        if (!w.has<WorldTransform2D>(e)) w.add<WorldTransform2D>(e);
        w.get<Transform2D>(e)->dirty = 1;
    };
    reg.add<Parent>("Parent", {MGE_FIELD(Parent, entity, Entity)});
    reg.add<Name>("Name", {MGE_FIELD(Name, text, Str32)});
}

// ------------------------------------------------------------------ binary save/load
std::vector<uint8_t> saveBinary(World& world, const Registry& reg) {
    Writer w;
    w.put("MGES", 4);
    w.u32(kFormatVersion);
    const auto& types = reg.types();
    w.u32(uint32_t(types.size()));
    for (const auto& t : types) {
        w.str(t.name);
        w.u16(uint16_t(t.fields.size()));
        for (const auto& f : t.fields) { w.str(f.name); w.u8(uint8_t(f.type)); }
    }
    auto ents = collect(world);
    Remap remap(ents);
    w.u32(uint32_t(ents.size()));
    for (Entity e : ents) {
        uint16_t n = 0;
        for (const auto& t : types) n += world.getRaw(e, t.id) ? 1 : 0;
        w.u16(n);
        for (uint16_t ti = 0; ti < types.size(); ++ti) {
            const auto& t = types[ti];
            auto* p = static_cast<const uint8_t*>(world.getRaw(e, t.id));
            if (!p) continue;
            w.u16(ti);
            for (const auto& f : t.fields) {
                if (f.type == FieldType::Entity) {
                    Entity ref;
                    std::memcpy(&ref, p + f.offset, sizeof ref);
                    w.u32(remap(world, ref));
                } else {
                    w.put(p + f.offset, fieldSize(f.type));
                }
            }
        }
    }
    return std::move(w.b);
}

bool loadBinary(World& world, const Registry& reg, const uint8_t* data, size_t size, std::vector<Entity>* created) {
    Reader r{data, data + size};
    char magic[4] = {};
    if (!r.get(magic, 4) || std::memcmp(magic, "MGES", 4) != 0) { MGE_LOGE("scene: not an MGE binary scene"); return false; }
    uint32_t version = r.u32();
    if (!r.ok || version < kMinFormatVersion || version > kFormatVersion) { MGE_LOGE("scene: unsupported version %u", version); return false; }

    struct SField { FieldType type; const Field* dst; };
    struct SType { const Registry::Type* dst; std::vector<SField> fields; };
    uint32_t typeCount = r.u32();
    if (!r.ok || typeCount > 4096) return false;
    std::vector<SType> schema(typeCount);
    for (auto& st : schema) {
        std::string name = r.str();
        st.dst = reg.find(name);
        if (!st.dst && r.ok) MGE_LOGW("scene: unknown component '%s' skipped", name.c_str());
        uint16_t fc = r.u16();
        for (uint16_t i = 0; i < fc && r.ok; ++i) {
            std::string fname = r.str();
            uint8_t ft = r.u8();
            if (ft > uint8_t(kLastFieldType)) { r.ok = false; break; }
            const Field* df = nullptr;
            if (st.dst)
                for (const auto& f : st.dst->fields)
                    if (fname == f.name && f.type == FieldType(ft)) df = &f;
            st.fields.push_back({FieldType(ft), df});
        }
        if (!r.ok) return false;
    }

    uint32_t entityCount = r.u32();
    if (!r.ok || entityCount > r.remaining() / 2) return false;   // each entity takes >= 2 bytes
    std::vector<Entity> made;
    made.reserve(entityCount);
    for (uint32_t i = 0; i < entityCount; ++i) made.push_back(world.create());

    for (uint32_t i = 0; i < entityCount; ++i) {
        uint16_t n = r.u16();
        for (uint16_t c = 0; c < n; ++c) {
            uint16_t ti = r.u16();
            if (!r.ok || ti >= typeCount) return rollback(world, made);
            const SType& st = schema[ti];
            uint8_t* dst = nullptr;
            if (st.dst) {
                dst = static_cast<uint8_t*>(world.addRaw(made[i], st.dst->id));
                std::memcpy(dst, st.dst->defaults.data(), st.dst->size);
            }
            for (const auto& sf : st.fields) {
                uint8_t buf[32];
                uint32_t bytes = diskSize(sf.type);
                if (!r.get(buf, bytes)) return rollback(world, made);
                if (!dst || !sf.dst) continue;
                if (sf.type == FieldType::Entity) {
                    uint32_t slot;
                    std::memcpy(&slot, buf, 4);
                    Entity ref = slot < entityCount ? made[slot] : Entity{};
                    std::memcpy(dst + sf.dst->offset, &ref, sizeof ref);
                } else {
                    if (sf.type == FieldType::Str32) buf[31] = 0;   // never trust the terminator on disk
                    std::memcpy(dst + sf.dst->offset, buf, bytes);
                }
            }
        }
        if (!r.ok) return rollback(world, made);
    }
    runHooks(world, reg, made);
    if (created) *created = std::move(made);
    return true;
}

// ------------------------------------------------------------------ JSON save/load
std::string saveJson(World& world, const Registry& reg) {
    auto ents = collect(world);
    Remap remap(ents);
    std::string out = "{\"version\":" + std::to_string(kFormatVersion) + ",\"entities\":[";
    for (uint32_t i = 0; i < ents.size(); ++i) {
        out += i ? ",\n  {" : "\n  {";
        out += "\"id\":" + std::to_string(i);
        for (const auto& t : reg.types()) {
            auto* p = static_cast<const uint8_t*>(world.getRaw(ents[i], t.id));
            if (!p) continue;
            out += ",\"" + t.name + "\":{";
            for (size_t j = 0; j < t.fields.size(); ++j) {
                const Field& f = t.fields[j];
                if (j) out += ",";
                out += "\"" + std::string(f.name) + "\":";
                const uint8_t* src = p + f.offset;
                switch (f.type) {
                    case FieldType::F32: { float v; std::memcpy(&v, src, 4); appendNum(out, v); break; }
                    case FieldType::I32: { int32_t v; std::memcpy(&v, src, 4); out += std::to_string(v); break; }
                    case FieldType::U32: { uint32_t v; std::memcpy(&v, src, 4); out += std::to_string(v); break; }
                    case FieldType::Vec2: {
                        float v[2]; std::memcpy(v, src, 8);
                        out += "["; appendNum(out, v[0]); out += ","; appendNum(out, v[1]); out += "]";
                        break;
                    }
                    case FieldType::Entity: {
                        Entity ref; std::memcpy(&ref, src, sizeof ref);
                        uint32_t slot = remap(world, ref);
                        out += slot == UINT32_MAX ? "null" : std::to_string(slot);
                        break;
                    }
                    case FieldType::Vec4: {
                        float v[4]; std::memcpy(v, src, 16);
                        out += "[";
                        for (int k = 0; k < 4; ++k) { if (k) out += ","; appendNum(out, v[k]); }
                        out += "]";
                        break;
                    }
                    case FieldType::I16: { int16_t v; std::memcpy(&v, src, 2); out += std::to_string(v); break; }
                    case FieldType::U16: { uint16_t v; std::memcpy(&v, src, 2); out += std::to_string(v); break; }
                    case FieldType::Str32: {
                        const char* c = reinterpret_cast<const char*>(src);
                        appendStr(out, std::string_view(c, ::strnlen(c, 32)));
                        break;
                    }
                }
            }
            out += "}";
        }
        out += "}";
    }
    out += "\n]}\n";
    return out;
}

bool loadJson(World& world, const Registry& reg, std::string_view json, std::vector<Entity>* created) {
    std::string buf(json);
    Parser ps{buf.data(), buf.data() + buf.size()};
    JVal root;
    if (!ps.val(root)) { MGE_LOGE("scene json: parse error at byte %d", int(ps.p - buf.data())); return false; }
    ps.ws();
    if (ps.p != ps.e) { MGE_LOGE("scene json: trailing data"); return false; }
    const JVal* ents = root.k == JVal::Obj ? root.get("entities") : nullptr;
    if (!ents || ents->k != JVal::Arr) { MGE_LOGE("scene json: missing \"entities\" array"); return false; }
    for (const auto& ev : ents->a) if (ev.k != JVal::Obj) return false;

    std::vector<Entity> made;
    std::unordered_map<long long, Entity> byId;
    for (const auto& ev : ents->a) {
        Entity e = world.create();
        made.push_back(e);
        if (const JVal* id = ev.get("id")) {
            if (id->k != JVal::Num) return rollback(world, made);
            byId[(long long)id->n] = e;
        }
    }

    for (size_t i = 0; i < ents->a.size(); ++i) {
        for (const auto& [key, cv] : ents->a[i].o) {
            if (key == "id") continue;
            const Registry::Type* t = reg.find(key);
            if (!t) { MGE_LOGW("scene json: unknown component '%s' skipped", key.c_str()); continue; }
            if (cv.k != JVal::Obj) return rollback(world, made);
            auto* dst = static_cast<uint8_t*>(world.addRaw(made[i], t->id));
            std::memcpy(dst, t->defaults.data(), t->size);
            for (const auto& [fname, fv] : cv.o) {
                const Field* f = nullptr;
                for (const auto& cand : t->fields) if (fname == cand.name) f = &cand;
                if (!f) { MGE_LOGW("scene json: unknown field %s.%s skipped", key.c_str(), fname.c_str()); continue; }
                uint8_t* out = dst + f->offset;
                switch (f->type) {
                    case FieldType::F32: {
                        if (fv.k != JVal::Num) return rollback(world, made);
                        float v = float(fv.n); std::memcpy(out, &v, 4); break;
                    }
                    case FieldType::I32: {
                        if (fv.k != JVal::Num) return rollback(world, made);
                        int32_t v = int32_t(fv.n); std::memcpy(out, &v, 4); break;
                    }
                    case FieldType::U32: {
                        if (fv.k != JVal::Num || fv.n < 0) return rollback(world, made);
                        uint32_t v = uint32_t(fv.n); std::memcpy(out, &v, 4); break;
                    }
                    case FieldType::Vec2: {
                        if (fv.k != JVal::Arr || fv.a.size() != 2 || fv.a[0].k != JVal::Num || fv.a[1].k != JVal::Num)
                            return rollback(world, made);
                        float v[2] = {float(fv.a[0].n), float(fv.a[1].n)};
                        std::memcpy(out, v, 8);
                        break;
                    }
                    case FieldType::Entity: {
                        Entity ref{};
                        if (fv.k == JVal::Num) {
                            auto it = byId.find((long long)fv.n);
                            if (it != byId.end()) ref = it->second;
                            else MGE_LOGW("scene json: %s.%s points at missing id", key.c_str(), fname.c_str());
                        } else if (fv.k != JVal::Null) {
                            return rollback(world, made);
                        }
                        std::memcpy(out, &ref, sizeof ref);
                        break;
                    }
                    case FieldType::Vec4: {
                        if (fv.k != JVal::Arr || fv.a.size() != 4) return rollback(world, made);
                        float v[4];
                        for (int k = 0; k < 4; ++k) {
                            if (fv.a[k].k != JVal::Num) return rollback(world, made);
                            v[k] = float(fv.a[k].n);
                        }
                        std::memcpy(out, v, 16);
                        break;
                    }
                    case FieldType::I16: {
                        if (fv.k != JVal::Num || fv.n < -32768 || fv.n > 32767) return rollback(world, made);
                        int16_t v = int16_t(fv.n); std::memcpy(out, &v, 2); break;
                    }
                    case FieldType::U16: {
                        if (fv.k != JVal::Num || fv.n < 0 || fv.n > 65535) return rollback(world, made);
                        uint16_t v = uint16_t(fv.n); std::memcpy(out, &v, 2); break;
                    }
                    case FieldType::Str32: {
                        if (fv.k != JVal::Str) return rollback(world, made);
                        scene2d::Name n = scene2d::Name::of(fv.s);
                        std::memcpy(out, n.text, 32);
                        break;
                    }
                }
            }
        }
    }
    runHooks(world, reg, made);
    if (created) *created = std::move(made);
    return true;
}

}  // namespace mge::serial
