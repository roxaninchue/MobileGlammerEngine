#include "mge/editor/inspector.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "mge/transform2d.h"

namespace mge::editor {
using serial::Field;
using serial::FieldHint;
using serial::FieldType;
using serial::Registry;

namespace {
std::string fmtFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.7g", double(v));
    return buf;
}

// Splits on spaces/commas; returns false when there are not exactly `n` numbers.
bool parseFloats(std::string_view text, float* out, int n) {
    std::string s(text);
    for (char& c : s) if (c == ',' || c == ';') c = ' ';
    const char* p = s.c_str();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p || !std::isfinite(v)) return false;
        out[i] = v;
        p = end;
    }
    while (*p == ' ' || *p == '\t') ++p;
    return *p == 0;
}

bool parseInt(std::string_view text, long long lo, long long hi, long long& out) {
    std::string s(text);
    const char* p = s.c_str();
    while (*p == ' ') ++p;
    char* end = nullptr;
    out = std::strtoll(p, &end, 10);
    if (end == p) return false;
    while (*end == ' ') ++end;
    return *end == 0 && out >= lo && out <= hi;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
}  // namespace

std::string Inspector::format(const Field& f, const uint8_t* bytes) {
    const uint8_t* src = bytes + f.offset;
    switch (f.type) {
        case FieldType::F32: {
            float v; std::memcpy(&v, src, 4);
            if (f.hint == FieldHint::Angle) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.6g", double(degrees(v)));
                return buf;
            }
            return fmtFloat(v);
        }
        case FieldType::I32: { int32_t v; std::memcpy(&v, src, 4); return std::to_string(v); }
        case FieldType::I16: { int16_t v; std::memcpy(&v, src, 2); return std::to_string(v); }
        case FieldType::U16: {
            uint16_t v; std::memcpy(&v, src, 2);
            if (f.hint == FieldHint::Bool) return v ? "true" : "false";
            return std::to_string(v);
        }
        case FieldType::U32: {
            uint32_t v; std::memcpy(&v, src, 4);
            if (f.hint == FieldHint::Color) {   // R in the low byte -> #RRGGBBAA
                char buf[16];
                std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
                return buf;
            }
            if (f.hint == FieldHint::Bool) return v ? "true" : "false";
            return std::to_string(v);
        }
        case FieldType::Vec2: { float v[2]; std::memcpy(v, src, 8); return fmtFloat(v[0]) + " " + fmtFloat(v[1]); }
        case FieldType::Vec4: {
            float v[4]; std::memcpy(v, src, 16);
            return fmtFloat(v[0]) + " " + fmtFloat(v[1]) + " " + fmtFloat(v[2]) + " " + fmtFloat(v[3]);
        }
        case FieldType::Entity: {
            ecs::Entity ref; std::memcpy(&ref, src, sizeof ref);   // SceneDoc encoding: index = uid
            return std::to_string(ref.index == UINT32_MAX ? 0u : ref.index);
        }
        case FieldType::Str32: {
            const char* c = reinterpret_cast<const char*>(src);
            return std::string(c, ::strnlen(c, 32));
        }
    }
    return {};
}

bool Inspector::parse(const Field& f, std::string_view text, uint8_t* bytes) {
    uint8_t* dst = bytes + f.offset;
    const bool boolText = text == "true" || text == "false";
    if (f.hint == FieldHint::Bool && boolText) text = text == "true" ? "1" : "0";
    long long i = 0;
    switch (f.type) {
        case FieldType::F32: {
            float v;
            if (!parseFloats(text, &v, 1)) return false;
            if (f.hint == FieldHint::Angle) v = radians(v);
            std::memcpy(dst, &v, 4);
            return true;
        }
        case FieldType::I32: {
            if (!parseInt(text, INT32_MIN, INT32_MAX, i)) return false;
            int32_t v = int32_t(i); std::memcpy(dst, &v, 4); return true;
        }
        case FieldType::I16: {
            if (!parseInt(text, INT16_MIN, INT16_MAX, i)) return false;
            int16_t v = int16_t(i); std::memcpy(dst, &v, 2); return true;
        }
        case FieldType::U16: {
            if (!parseInt(text, 0, UINT16_MAX, i)) return false;
            uint16_t v = uint16_t(i); std::memcpy(dst, &v, 2); return true;
        }
        case FieldType::U32: {
            if (f.hint == FieldHint::Color) {
                if (!text.empty() && text[0] == '#') text.remove_prefix(1);
                if (text.size() != 6 && text.size() != 8) return false;
                uint8_t c[4] = {0, 0, 0, 0xFF};
                for (size_t k = 0; k < text.size() / 2; ++k) {
                    const int hi = hexDigit(text[2 * k]), lo = hexDigit(text[2 * k + 1]);
                    if (hi < 0 || lo < 0) return false;
                    c[k] = uint8_t(hi * 16 + lo);
                }
                uint32_t v = uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
                std::memcpy(dst, &v, 4);
                return true;
            }
            if (!parseInt(text, 0, UINT32_MAX, i)) return false;
            uint32_t v = uint32_t(i); std::memcpy(dst, &v, 4); return true;
        }
        case FieldType::Vec2: {
            float v[2];
            if (!parseFloats(text, v, 2)) return false;
            std::memcpy(dst, v, 8);
            return true;
        }
        case FieldType::Vec4: {
            float v[4];
            if (!parseFloats(text, v, 4)) return false;
            std::memcpy(dst, v, 16);
            return true;
        }
        case FieldType::Entity: {
            if (!parseInt(text, 0, UINT32_MAX - 1, i)) return false;
            ecs::Entity ref{i == 0 ? UINT32_MAX : uint32_t(i), 0};
            std::memcpy(dst, &ref, sizeof ref);
            return true;
        }
        case FieldType::Str32: {
            scene2d::Name n = scene2d::Name::of(text);
            std::memcpy(dst, n.text, 32);
            return true;
        }
    }
    return false;
}

std::vector<InspectorComponent> Inspector::inspect(Uid uid) const {
    std::vector<InspectorComponent> out;
    if (!doc_.entity(uid).valid()) return out;
    for (const Registry::Type& t : doc_.registry().types()) {
        if (t.editorHidden) continue;
        std::vector<uint8_t> bytes = doc_.readComponent(uid, t);
        if (bytes.empty()) continue;
        InspectorComponent c;
        c.type = t.name;
        c.removable = t.name != "Transform2D";
        for (const Field& f : t.fields) {
            if (f.hint == FieldHint::Hidden) continue;
            c.fields.push_back({f.name, f.type, f.hint, format(f, bytes.data())});
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> Inspector::addable(Uid uid) const {
    std::vector<std::string> out;
    ecs::Entity e = doc_.entity(uid);
    if (!e.valid()) return out;
    for (const Registry::Type& t : doc_.registry().types())
        if (!t.editorHidden && !doc_.world().getRaw(e, t.id)) out.push_back(t.name);
    return out;
}

bool Inspector::setField(Uid uid, std::string_view type, std::string_view field, std::string_view value, uint64_t scrub) {
    const Registry::Type* t = doc_.registry().find(type);
    if (!t || t->editorHidden) return false;
    std::vector<uint8_t> bytes = doc_.readComponent(uid, *t);
    if (bytes.empty()) return false;
    for (const Field& f : t->fields) {
        if (field != f.name) continue;
        if (!parse(f, value, bytes.data())) return false;
        auto cmd = std::make_unique<SetComponentCommand>(doc_, uid, *t, std::move(bytes),
                                                         "Edit " + t->name + "." + f.name);
        if (scrub) cmd->setMergeKey(mix(mix(mix(scrub, uid), t->id), f.offset) | 1);
        return stack_.execute(std::move(cmd));
    }
    return false;
}

bool Inspector::addComponent(Uid uid, std::string_view type) {
    const Registry::Type* t = doc_.registry().find(type);
    ecs::Entity e = doc_.entity(uid);
    if (!t || t->editorHidden || !e.valid() || doc_.world().getRaw(e, t->id)) return false;
    return stack_.execute(std::make_unique<SetComponentCommand>(doc_, uid, *t, t->defaults, "Add " + t->name));
}

bool Inspector::removeComponent(Uid uid, std::string_view type) {
    const Registry::Type* t = doc_.registry().find(type);
    if (!t || t->editorHidden || t->name == "Transform2D") return false;
    return stack_.execute(std::make_unique<RemoveComponentCommand>(uid, *t));
}

}  // namespace mge::editor
