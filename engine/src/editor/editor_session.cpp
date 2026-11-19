#include "mge/editor/editor_session.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "mge/log.h"

namespace mge::editor {
using ecs::Entity;
using render::Sprite;
using render::SpriteRenderer;
using scene2d::Affine2;
using scene2d::WorldTransform2D;

void appendJsonString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
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

namespace {
const char* typeName(serial::FieldType t) {
    switch (t) {
        case serial::FieldType::F32: return "F32";
        case serial::FieldType::I32: return "I32";
        case serial::FieldType::U32: return "U32";
        case serial::FieldType::Vec2: return "Vec2";
        case serial::FieldType::Entity: return "Entity";
        case serial::FieldType::Vec4: return "Vec4";
        case serial::FieldType::I16: return "I16";
        case serial::FieldType::U16: return "U16";
        case serial::FieldType::Str32: return "Str32";
    }
    return "?";
}
const char* hintName(serial::FieldHint h) {
    switch (h) {
        case serial::FieldHint::Angle: return "Angle";
        case serial::FieldHint::Color: return "Color";
        case serial::FieldHint::Bool: return "Bool";
        case serial::FieldHint::Hidden: return "Hidden";
        case serial::FieldHint::Enum: return "Enum";
        default: return "";
    }
}

// Splits the next space-separated token off `s`.
std::string_view token(std::string_view& s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    size_t n = s.find(' ');
    std::string_view t = s.substr(0, n);
    s.remove_prefix(n == std::string_view::npos ? s.size() : n);
    return t;
}
std::string_view rest(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}
bool number(std::string_view t, long long& out) {
    if (t.empty() || t.size() > 20) return false;
    std::string s(t);
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, 10);
    return end && *end == 0;
}
bool uidArg(std::string_view& s, Uid& out) {
    long long v = 0;
    if (!number(token(s), v) || v < 0 || v > UINT32_MAX) return false;
    out = Uid(v);
    return true;
}

// Thin quad from a to b, `width` world units thick.
void line(render::SpriteBatcher& b, vec2 a, vec2 c, float width, uint32_t color, int16_t layer) {
    const vec2 d = c - a;
    const float len = length(d);
    if (len <= 0) return;
    Sprite s;
    s.world = Affine2::trs((a + c) * 0.5f, std::atan2(d.y, d.x), {1, 1});
    s.size = {len + width, width};
    s.color = color;
    s.layer = layer;
    b.submit(s);
}
}  // namespace

EditorSession::EditorSession() {
    serial::registerScene2D(reg_);
    render::registerRender2D(reg_);
    physics2d::registerPhysics2D(reg_);
    registerEditor(reg_);
    rebuild();
}

EditorSession::~EditorSession() = default;

void EditorSession::rebuild() {
    const uint64_t rev = world_ ? revision() + 1 : 0;
    if (world_ && play_.active()) play_.stop(*this);
    inspector_.reset();
    hierarchy_.reset();
    stack_.reset();
    doc_.reset();
    transforms_.reset();
    world_.reset();
    world_ = std::make_unique<ecs::World>();
    transforms_ = std::make_unique<scene2d::TransformSystem2D>();
    doc_ = std::make_unique<SceneDoc>(*world_, reg_, transforms_.get());
    stack_ = std::make_unique<CommandStack>(*doc_);
    hierarchy_ = std::make_unique<Hierarchy>(*stack_);
    inspector_ = std::make_unique<Inspector>(*stack_);
    selection_.clear();
    selRev_ = rev;   // revision() = selRev_ + doc + stack, all fresh: starts above the old value
}

void EditorSession::newScene(bool starter) {
    rebuild();
    if (starter) buildStarter();
}

void EditorSession::buildStarter() {
    // A tiny scene to poke at: ground, a player with a child, two crates.
    auto tint = [&](Uid u, const char* color, vec2 size) {
        inspector_->setField(u, "SpriteRenderer", "color", color);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g %g", double(size.x), double(size.y));
        inspector_->setField(u, "SpriteRenderer", "size", buf);
    };
    // Press play: the crates fall and tumble onto the ground (Collider2D on each).
    auto body = [&](Uid u, const char* type, const char* size) {
        inspector_->addComponent(u, "Collider2D");
        inspector_->setField(u, "Collider2D", "bodyType", type);
        inspector_->setField(u, "Collider2D", "size", size);
    };
    Uid ground = hierarchy_->create("Ground", kNoUid, {0, -3.5f});
    tint(ground, "#3A4A5CFF", {14, 1});
    body(ground, "Static", "14 1");
    Uid player = hierarchy_->create("Player", kNoUid, {-3, -2.5f});
    tint(player, "#4FC3F7FF", {1, 1});
    body(player, "Dynamic", "1 1");
    inspector_->setField(player, "Collider2D", "fixedRotation", "true");
    Uid hat = hierarchy_->create("Hat", player, {0, 0.7f});
    tint(hat, "#FFB74DFF", {0.8f, 0.3f});
    Uid crate = hierarchy_->create("Crate", kNoUid, {1.5f, 0.5f});
    tint(crate, "#A1887FFF", {1, 1});
    body(crate, "Dynamic", "1 1");
    inspector_->setField(crate, "Transform2D", "rotation", "20");
    std::vector<Uid> copy = hierarchy_->duplicate({crate});
    if (!copy.empty()) inspector_->setField(copy[0], "Transform2D", "position", "2 2.5");
    stack_->clear();   // the starter scene is the starting point, not history
    stack_->markSaved();
    selection_.clear();
    ++selRev_;
}

void EditorSession::select(Uid uid, bool toggle) {
    if (toggle) selection_.toggle(uid);
    else selection_.set(uid);
    if (uid != kNoUid) hierarchy_->expandTo(uid);
    ++selRev_;
}

Uid EditorSession::pick(vec2 p) {
    Uid best = kNoUid;
    int best_layer = INT32_MIN;
    world_->each<EditorId, WorldTransform2D, SpriteRenderer>([&](Entity, EditorId& id, WorldTransform2D& wt, SpriteRenderer& sr) {
        if (!sr.visible) return;
        const vec2 local = wt.m.inverse().apply(p);
        const vec2 lo = -sr.pivot * sr.size, hi = (vec2{1, 1} - sr.pivot) * sr.size;
        if (local.x < min(lo.x, hi.x) || local.x > max(lo.x, hi.x) || local.y < min(lo.y, hi.y) || local.y > max(lo.y, hi.y))
            return;
        if (sr.layer > best_layer || (sr.layer == best_layer && id.uid > best)) {
            best_layer = sr.layer;
            best = id.uid;
        }
    });
    return best;
}

bool EditorSession::tick(float dt) {
    if (!play_.update(*this, dt)) return false;
    // Panels show live values while playing, refreshed four times a second (not every frame).
    const int quarter = int(play_.time() * 4.0f);
    if (quarter != playTick_) {
        playTick_ = quarter;
        ++selRev_;
    }
    return true;
}

void EditorSession::update() {
    selection_.prune(*doc_);
    transforms_->update(*world_);
}

void EditorSession::draw(render::SpriteBatcher& batcher, const render::ScreenLayout& view, bool overlay) {
    render::submitSprites(*world_, batcher);
    const float ppu = view.pixelsPerUnit;
    if (!overlay || ppu <= 0) return;
    const float px = 1.0f / ppu;
    // Grid: one line per step, coarser when zoomed out so there are never more than ~120.
    const render::Rect bounds = batcher.camera().bounds();
    float step = gridStep > 0 ? gridStep : 1.0f;
    while ((bounds.size().x + bounds.size().y) / step > 120.0f) step *= 2.0f;
    const uint32_t minor = render::packColor({1, 1, 1, 0.06f}), axis = render::packColor({1, 1, 1, 0.22f});
    for (float x = std::floor(bounds.min.x / step) * step; x <= bounds.max.x; x += step)
        line(batcher, {x, bounds.min.y}, {x, bounds.max.y}, px, std::fabs(x) < step * 0.5f ? axis : minor, -30000);
    for (float y = std::floor(bounds.min.y / step) * step; y <= bounds.max.y; y += step)
        line(batcher, {bounds.min.x, y}, {bounds.max.x, y}, px, std::fabs(y) < step * 0.5f ? axis : minor, -30000);
    // Selection: outline of each selected sprite (or a cross at the origin without one).
    const uint32_t primary = render::packColor({1.0f, 0.6f, 0.1f, 1}), secondary = render::packColor({1.0f, 0.85f, 0.4f, 0.8f});
    for (Uid u : selection_.all()) {
        Entity e = doc_->entity(u);
        const WorldTransform2D* wt = e.valid() ? world_->get<WorldTransform2D>(e) : nullptr;
        if (!wt) continue;
        const uint32_t c = u == selection_.primary() ? primary : secondary;
        if (const SpriteRenderer* sr = world_->get<SpriteRenderer>(e)) {
            const vec2 lo = -sr->pivot * sr->size, hi = (vec2{1, 1} - sr->pivot) * sr->size;
            const vec2 q[4] = {wt->m.apply(lo), wt->m.apply({hi.x, lo.y}), wt->m.apply(hi), wt->m.apply({lo.x, hi.y})};
            for (int i = 0; i < 4; ++i) line(batcher, q[i], q[(i + 1) % 4], 2 * px, c, 30000);
        } else {
            const vec2 o = wt->m.translation();
            line(batcher, o - vec2{8 * px, 0}, o + vec2{8 * px, 0}, 2 * px, c, 30000);
            line(batcher, o - vec2{0, 8 * px}, o + vec2{0, 8 * px}, 2 * px, c, 30000);
        }
    }
    // Gizmo on the primary selection (not while playing: the scene is not editable then).
    const Uid p = selection_.primary();
    if (p != kNoUid && !stack_->locked() && world_->has<scene2d::Transform2D>(doc_->entity(p)))
        gizmo_.draw(batcher, view, hierarchy_->worldMatrix(p));
}

bool EditorSession::handle(std::string_view line) {
    std::string_view s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    const std::string_view cmd = token(s);
    Uid id = kNoUid, other = kNoUid;
    long long n = 0;
    if (cmd == "play" || cmd == "pause" || cmd == "step" || cmd == "stop") {
        bool ok = cmd == "play" ? play_.play(*this) : cmd == "pause" ? play_.pause()
                : cmd == "step" ? play_.step(*this) : play_.stop(*this);
        if (ok) { ++selRev_; playTick_ = -1; }
        return ok;
    }
    if (cmd == "undo") return stack_->undo();
    if (cmd == "redo") return stack_->redo();
    if (cmd == "seal") { stack_->seal(); return true; }
    if (cmd == "select" || cmd == "toggle") {
        if (!uidArg(s, id) || (id != kNoUid && !doc_->entity(id).valid())) return false;
        select(id, cmd == "toggle");
        return true;
    }
    if (cmd == "expand") {
        if (!uidArg(s, id) || !number(token(s), n)) return false;
        hierarchy_->setExpanded(id, n != 0);
        return true;
    }
    if (cmd == "create") {
        if (!uidArg(s, id)) return false;
        std::string_view name = rest(s);
        Uid made = hierarchy_->create(name.empty() ? "Entity" : name, id);
        if (made == kNoUid) return false;
        select(made);
        return true;
    }
    if (cmd == "rename") {
        if (!uidArg(s, id)) return false;
        std::string_view name = rest(s);
        return !name.empty() && hierarchy_->rename(id, name);
    }
    if (cmd == "duplicate") {
        std::vector<Uid> made = hierarchy_->duplicate(selection_.all());
        if (made.empty()) return false;
        selection_.clear();
        for (Uid u : made) selection_.toggle(u);
        ++selRev_;
        return true;
    }
    if (cmd == "delete") {
        if (!hierarchy_->remove(selection_.all())) return false;
        selection_.clear();
        ++selRev_;
        return true;
    }
    if (cmd == "reparent" || cmd == "reorder") {
        if (!uidArg(s, id)) return false;
        if (cmd == "reparent" && !uidArg(s, other)) return false;
        if (!number(token(s), n) || n < -1 || n > INT32_MAX) return false;
        return cmd == "reparent" ? hierarchy_->reparent(id, other, int(n)) : hierarchy_->reorder(id, int(n));
    }
    if (cmd == "set" || cmd == "scrub") {
        long long key = 0;
        if (cmd == "scrub" && (!number(token(s), key) || key <= 0)) return false;
        if (!uidArg(s, id)) return false;
        const std::string_view type = token(s), field = token(s);
        return inspector_->setField(id, type, field, rest(s), uint64_t(key));
    }
    if (cmd == "gizmo") {
        const std::string_view m = token(s);
        if (m == "move") gizmo_.mode = GizmoMode::Move;
        else if (m == "rotate") gizmo_.mode = GizmoMode::Rotate;
        else if (m == "scale") gizmo_.mode = GizmoMode::Scale;
        else return false;
        ++selRev_;
        return true;
    }
    if (cmd == "space") {
        const std::string_view m = token(s);
        if (m != "world" && m != "local") return false;
        gizmo_.localSpace = m == "local";
        ++selRev_;
        return true;
    }
    if (cmd == "snap") {
        if (!number(token(s), n)) return false;
        SnapSettings snap = gizmo_.snap;
        snap.enabled = n != 0;
        float* opt[3] = {&snap.grid, &snap.angleDeg, &snap.scaleStep};
        for (float* o : opt) {
            const std::string_view t = token(s);
            if (t.empty()) break;
            std::string tmp(t);
            char* end = nullptr;
            const float v = std::strtof(tmp.c_str(), &end);
            if (!end || *end || !(v > 0) || !std::isfinite(v)) return false;
            *o = v;
        }
        gizmo_.snap = snap;
        ++selRev_;
        return true;
    }
    if (cmd == "add" || cmd == "remove") {
        if (!uidArg(s, id)) return false;
        const std::string_view type = token(s);
        return cmd == "add" ? inspector_->addComponent(id, type) : inspector_->removeComponent(id, type);
    }
    MGE_LOGW("editor: unknown command '%.*s'", int(cmd.size()), cmd.data());
    return false;
}

std::string EditorSession::stateJson() {
    selection_.prune(*doc_);
    std::string o;
    o.reserve(4096);
    o += "{\"rev\":" + std::to_string(revision());
    o += ",\"undo\":";
    appendJsonString(o, stack_->canUndo() ? stack_->undoLabel() : std::string());
    o += ",\"redo\":";
    appendJsonString(o, stack_->canRedo() ? stack_->redoLabel() : std::string());
    o += ",\"dirty\":";
    o += stack_->isDirty() ? "true" : "false";
    o += ",\"locked\":";
    o += stack_->locked() ? "true" : "false";
    {
        const char* modes[] = {"move", "rotate", "scale"};
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      ",\"gizmo\":{\"mode\":\"%s\",\"local\":%s,\"snap\":%s,\"grid\":%g,\"angle\":%g,\"scaleStep\":%g}",
                      modes[int(gizmo_.mode)], gizmo_.localSpace ? "true" : "false", gizmo_.snap.enabled ? "true" : "false",
                      double(gizmo_.snap.grid), double(gizmo_.snap.angleDeg), double(gizmo_.snap.scaleStep));
        o += buf;
    }
    {
        const char* states[] = {"editing", "playing", "paused"};
        char buf[128];
        std::snprintf(buf, sizeof buf, ",\"play\":{\"state\":\"%s\",\"time\":%.2f,\"steps\":%u}",
                      states[int(play_.state())], double(play_.time()), play_.steps());
        o += buf;
    }
    o += ",\"rows\":[";
    bool first = true;
    for (const TreeRow& r : hierarchy_->rows(selection_)) {
        if (!first) o += ',';
        first = false;
        o += "{\"id\":" + std::to_string(r.uid) + ",\"p\":" + std::to_string(r.parent) + ",\"d\":" + std::to_string(r.depth) +
             ",\"c\":" + std::to_string(r.childCount) + ",\"x\":" + (r.expanded ? "true" : "false") +
             ",\"s\":" + (r.selected ? "true" : "false") + ",\"n\":";
        appendJsonString(o, r.name);
        o += '}';
    }
    o += "],\"sel\":[";
    for (size_t i = 0; i < selection_.all().size(); ++i) {
        if (i) o += ',';
        o += std::to_string(selection_.all()[i]);
    }
    o += "],\"insp\":";
    const Uid p = selection_.primary();
    if (p == kNoUid) {
        o += "null";
    } else {
        o += "{\"id\":" + std::to_string(p) + ",\"name\":";
        appendJsonString(o, hierarchy_->nameOf(p));
        o += ",\"components\":[";
        bool fc = true;
        for (const InspectorComponent& c : inspector_->inspect(p)) {
            if (!fc) o += ',';
            fc = false;
            o += "{\"type\":";
            appendJsonString(o, c.type);
            o += ",\"removable\":";
            o += c.removable ? "true" : "false";
            o += ",\"fields\":[";
            for (size_t i = 0; i < c.fields.size(); ++i) {
                const InspectorField& f = c.fields[i];
                if (i) o += ',';
                o += "{\"name\":";
                appendJsonString(o, f.name);
                o += ",\"type\":\"";
                o += typeName(f.type);
                o += "\",\"hint\":\"";
                o += hintName(f.hint);
                o += "\",\"value\":";
                appendJsonString(o, f.value);
                if (!f.options.empty()) {
                    o += ",\"options\":[";
                    for (size_t k = 0; k < f.options.size(); ++k) {
                        if (k) o += ',';
                        appendJsonString(o, f.options[k]);
                    }
                    o += ']';
                }
                o += '}';
            }
            o += "]}";
        }
        o += "],\"addable\":[";
        bool fa = true;
        for (const std::string& t : inspector_->addable(p)) {
            if (!fa) o += ',';
            fa = false;
            appendJsonString(o, t);
        }
        o += "]}";
    }
    o += '}';
    return o;
}

}  // namespace mge::editor
