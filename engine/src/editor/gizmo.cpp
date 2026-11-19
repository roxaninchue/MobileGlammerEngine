#include "mge/editor/gizmo.h"
#include <cmath>
#include "mge/editor/editor_session.h"

namespace mge::editor {
using scene2d::Affine2;
using scene2d::Transform2D;

namespace {
constexpr uint32_t kColX = 0xFF3539E5u;      // packed RGBA, R in the low byte: #E53935
constexpr uint32_t kColY = 0xFF47A043u;      // #43A047
constexpr uint32_t kColFree = 0xFF26A7FFu;   // #FFA726
constexpr uint32_t kColRing = 0xFFF5A542u;   // #42A5F5
constexpr uint32_t kColActive = 0xFFFFFFFFu;

float wrapPi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}

float distToSegment(vec2 p, vec2 a, vec2 b) {
    const vec2 ab = b - a;
    const float len2 = lengthSq(ab);
    const float t = len2 > 0 ? clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

// Screen direction (unit, px space) of a world direction at world point p.
vec2 screenDir(const render::ScreenLayout& v, vec2 p, vec2 d) {
    return normalize(v.worldToScreen(p + d) - v.worldToScreen(p));
}

const serial::Registry::Type& transformType(EditorSession& s) { return *s.registry().find("Transform2D"); }

Transform2D readT(const std::vector<uint8_t>& b) {
    Transform2D t;
    std::memcpy(&t, b.data(), sizeof t);
    return t;
}

struct Painter {
    render::SpriteBatcher& b;
    const render::ScreenLayout& v;
    float worldPerPx;
    // Quad between two screen points, `w` px wide.
    void line(vec2 a, vec2 c, float w, uint32_t color) const {
        const vec2 wa = v.screenToWorld(a), wc = v.screenToWorld(c);
        const vec2 d = wc - wa;
        const float len = length(d);
        if (len <= 0) return;
        render::Sprite s;
        s.world = Affine2::trs((wa + wc) * 0.5f, std::atan2(d.y, d.x), {1, 1});
        s.size = {len + w * worldPerPx, w * worldPerPx};
        s.color = color;
        s.layer = 31000;
        b.submit(s);
    }
    // Filled square centred on a screen point, `size` px, rotated by `angle` (world radians).
    void box(vec2 c, float size, float angle, uint32_t color) const {
        render::Sprite s;
        s.world = Affine2::trs(v.screenToWorld(c), angle, {1, 1});
        s.size = {size * worldPerPx, size * worldPerPx};
        s.color = color;
        s.layer = 31001;
        b.submit(s);
    }
};
}  // namespace

vec2 Gizmo::axisDir(const Affine2& target, int axis, bool local) const {
    if (!local) return axis == 0 ? vec2{1, 0} : vec2{0, 1};
    const vec2 d = axis == 0 ? vec2{target.a, target.b} : vec2{target.c, target.d};
    const vec2 n = normalize(d);
    return lengthSq(n) > 0 ? n : (axis == 0 ? vec2{1, 0} : vec2{0, 1});
}

GizmoHandle Gizmo::hitTest(const render::ScreenLayout& view, const Affine2& target, vec2 px) const {
    const vec2 pivot = target.translation();
    const vec2 o = view.worldToScreen(pivot);
    const float r = style.hitRadiusDp * density, centre = style.centreSizeDp * density;
    const float L = style.axisLengthDp * density;
    if (length(px - o) <= max(centre, r * 0.8f)) return mode == GizmoMode::Scale ? GizmoHandle::ScaleUniform : GizmoHandle::MoveFree;
    if (mode == GizmoMode::Rotate) {
        const float R = style.ringRadiusDp * density;
        return std::fabs(length(px - o) - R) <= r ? GizmoHandle::Rotate : GizmoHandle::None;
    }
    const bool local = mode == GizmoMode::Scale || localSpace;
    GizmoHandle best = GizmoHandle::None;
    float bestDist = r;
    for (int axis = 0; axis < 2; ++axis) {
        const vec2 s = screenDir(view, pivot, axisDir(target, axis, local));
        const vec2 tip = o + s * L;
        float d = length(px - tip);
        if (mode == GizmoMode::Move) d = min(d, distToSegment(px, o + s * centre, tip) / 0.7f);
        if (d <= bestDist) {
            bestDist = d;
            best = mode == GizmoMode::Move ? (axis ? GizmoHandle::MoveY : GizmoHandle::MoveX)
                                           : (axis ? GizmoHandle::ScaleY : GizmoHandle::ScaleX);
        }
    }
    return best;
}

bool Gizmo::begin(EditorSession& session, const render::ScreenLayout& view, vec2 px) {
    active_ = GizmoHandle::None;
    starts_.clear();
    const Uid primary = session.selection().primary();
    if (primary == kNoUid || session.stack().locked()) return false;
    SceneDoc& doc = session.doc();
    const auto& tt = transformType(session);
    if (doc.readComponent(primary, tt).empty()) return false;
    target_ = session.hierarchy().worldMatrix(primary);
    const GizmoHandle h = hitTest(view, target_, px);
    if (h == GizmoHandle::None) return false;

    // Everything selected that has a transform, minus entities whose ancestor is selected too
    // (they follow their parent). The primary goes first.
    std::vector<Uid> uids{primary};
    for (Uid u : session.selection().all()) if (u != primary) uids.push_back(u);
    for (Uid u : uids) {
        bool covered = false;
        for (Uid o : uids) covered |= o != u && doc.isAncestor(o, u);
        if (covered) continue;
        std::vector<uint8_t> b = doc.readComponent(u, tt);
        if (b.empty()) continue;
        const Uid parent = doc.parentOf(u);
        starts_.push_back({u, std::move(b), session.hierarchy().worldMatrix(u),
                           parent ? session.hierarchy().worldMatrix(parent) : Affine2{}});
    }
    active_ = h;
    moved_ = false;
    startPx_ = px;
    startWorld_ = view.screenToWorld(px);
    pivot_ = target_.translation();
    startAngle_ = std::atan2(startWorld_.y - pivot_.y, startWorld_.x - pivot_.x);
    lastAngle_ = startAngle_;
    totalAngle_ = 0;
    curAngle_ = 0;
    const int axis = (h == GizmoHandle::MoveY || h == GizmoHandle::ScaleY) ? 1 : 0;
    axis_ = axisDir(target_, axis, h == GizmoHandle::ScaleX || h == GizmoHandle::ScaleY || localSpace);
    startOriginPx_ = view.worldToScreen(pivot_);
    session.stack().seal();
    return true;
}

void Gizmo::drag(EditorSession& session, const render::ScreenLayout& view, vec2 px) {
    if (!active()) return;
    const vec2 w = view.screenToWorld(px);
    Affine2 delta;
    vec2 factor{1, 1};
    switch (active_) {
        case GizmoHandle::MoveFree: {
            vec2 d = w - startWorld_;
            if (snap.enabled) d = vec2{snapTo(pivot_.x + d.x, snap.grid), snapTo(pivot_.y + d.y, snap.grid)} - pivot_;
            delta.tx = d.x;
            delta.ty = d.y;
            break;
        }
        case GizmoHandle::MoveX:
        case GizmoHandle::MoveY: {
            float t = dot(w - startWorld_, axis_);
            if (snap.enabled) {
                if (!localSpace) {   // world axis: land the coordinate on the grid
                    const float c = active_ == GizmoHandle::MoveX ? pivot_.x : pivot_.y;
                    t = snapTo(c + t, snap.grid) - c;
                } else {
                    t = snapTo(t, snap.grid);
                }
            }
            delta.tx = axis_.x * t;
            delta.ty = axis_.y * t;
            break;
        }
        case GizmoHandle::Rotate: {
            // Accumulate unwrapped so several turns work.
            const float a = std::atan2(w.y - pivot_.y, w.x - pivot_.x);
            totalAngle_ += wrapPi(a - lastAngle_);
            lastAngle_ = a;
            float angle = totalAngle_;
            if (snap.enabled && snap.angleDeg > 0 && !starts_.empty()) {
                // Snap the primary's resulting rotation, not the delta (15 deg steps land on 15, 30...).
                const float r0 = readT(starts_[0].bytes).rotation;
                const float sign = (starts_[0].parentWorld.a * starts_[0].parentWorld.d -
                                    starts_[0].parentWorld.b * starts_[0].parentWorld.c) < 0 ? -1.0f : 1.0f;
                angle = (snapTo(r0 + sign * angle, radians(snap.angleDeg)) - r0) * sign;
            }
            curAngle_ = angle;
            const Affine2 rot = Affine2::trs({}, angle, {1, 1});
            delta = Affine2::trs(pivot_, 0, {1, 1}) * rot * Affine2::trs(-pivot_, 0, {1, 1});
            break;
        }
        case GizmoHandle::ScaleX:
        case GizmoHandle::ScaleY:
        case GizmoHandle::ScaleUniform: {
            const vec2 a0 = startPx_ - startOriginPx_, a1 = px - startOriginPx_;
            float f = 1.0f;
            if (active_ == GizmoHandle::ScaleUniform) {
                const float l0 = length(a0);
                f = l0 > 4.0f ? length(a1) / l0 : 1.0f;
            } else {
                const vec2 sd = screenDir(view, pivot_, axis_);
                const float d0 = dot(a0, sd);
                f = std::fabs(d0) > 4.0f ? dot(a1, sd) / d0 : 1.0f;
            }
            factor = active_ == GizmoHandle::ScaleX ? vec2{f, 1} : active_ == GizmoHandle::ScaleY ? vec2{1, f} : vec2{f, f};
            break;
        }
        default: return;
    }
    apply(session, delta, factor);
}

void Gizmo::apply(EditorSession& session, const Affine2& delta, vec2 factor) {
    SceneDoc& doc = session.doc();
    const auto& tt = transformType(session);
    const bool scaling = active_ == GizmoHandle::ScaleX || active_ == GizmoHandle::ScaleY || active_ == GizmoHandle::ScaleUniform;
    const bool rotating = active_ == GizmoHandle::Rotate;
    for (const Start& s : starts_) {
        Transform2D t = readT(s.bytes);
        if (scaling) {
            t.scale = t.scale * factor;
            if (snap.enabled && snap.scaleStep > 0)
                for (int i = 0; i < 2; ++i) {
                    float v = snapTo(t.scale[i], snap.scaleStep);
                    if (v == 0) v = t.scale[i] < 0 ? -snap.scaleStep : snap.scaleStep;   // never collapse to 0
                    t.scale[i] = v;
                }
        } else {
            const Affine2 w = delta * s.world;
            t.position = s.parentWorld.inverse().apply(w.translation());
            if (rotating) {
                const float det = s.parentWorld.a * s.parentWorld.d - s.parentWorld.b * s.parentWorld.c;
                t.rotation += det < 0 ? -curAngle_ : curAngle_;   // unwrapped: several turns add up
            }
        }
        t.dirty = 1;
        // Back where it started (e.g. snapped to the start cell): use the exact start bytes, so
        // float noise never records a step.
        const Transform2D t0 = readT(s.bytes);
        const bool same = lengthSq(t.position - t0.position) < 1e-10f && std::fabs(t.rotation - t0.rotation) < 1e-6f &&
                          lengthSq(t.scale - t0.scale) < 1e-10f;
        std::vector<uint8_t> b(s.bytes.size());
        if (same) b = s.bytes;
        else std::memcpy(b.data(), &t, sizeof t);
        if (b != doc.readComponent(s.uid, tt)) {
            doc.writeComponent(s.uid, tt, b);
            moved_ = true;
        }
    }
}

void Gizmo::end(EditorSession& session) {
    if (!active()) return;
    const GizmoHandle h = active_;
    active_ = GizmoHandle::None;
    if (!moved_) { starts_.clear(); return; }
    SceneDoc& doc = session.doc();
    const auto& tt = transformType(session);
    const char* verb = h == GizmoHandle::Rotate ? "Rotate" : (h == GizmoHandle::ScaleX || h == GizmoHandle::ScaleY ||
                                                               h == GizmoHandle::ScaleUniform) ? "Scale" : "Move";
    const std::string label = starts_.size() == 1 ? std::string(verb) + " " + session.hierarchy().nameOf(starts_[0].uid)
                                                  : std::string(verb) + " " + std::to_string(starts_.size());
    CommandStack& stack = session.stack();
    stack.begin(label);
    for (Start& s : starts_) {
        std::vector<uint8_t> after = doc.readComponent(s.uid, tt);
        if (after.empty() || after == s.bytes) continue;
        stack.execute(std::make_unique<SetComponentCommand>(s.uid, tt, std::move(s.bytes), std::move(after), label));
    }
    stack.end();
    starts_.clear();
}

void Gizmo::cancel(EditorSession& session) {
    if (!active()) return;
    active_ = GizmoHandle::None;
    const auto& tt = transformType(session);
    for (const Start& s : starts_) session.doc().writeComponent(s.uid, tt, s.bytes);
    starts_.clear();
}

void Gizmo::draw(render::SpriteBatcher& batcher, const render::ScreenLayout& view, const Affine2& target) const {
    if (view.pixelsPerUnit <= 0) return;
    const Painter p{batcher, view, 1.0f / view.pixelsPerUnit};
    const vec2 pivot = target.translation();
    const vec2 o = view.worldToScreen(pivot);
    const float L = style.axisLengthDp * density, lw = style.lineWidthDp * density;
    const float tip = style.tipSizeDp * density, centre = style.centreSizeDp * density;
    auto col = [&](GizmoHandle h, uint32_t c) { return active_ == h ? kColActive : c; };

    if (mode == GizmoMode::Rotate) {
        const float R = style.ringRadiusDp * density;
        constexpr int kSegments = 48;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = 2 * kPi * float(i) / kSegments, a1 = 2 * kPi * float(i + 1) / kSegments;
            p.line(o + vec2{std::cos(a0), std::sin(a0)} * R, o + vec2{std::cos(a1), std::sin(a1)} * R, lw,
                   col(GizmoHandle::Rotate, kColRing));
        }
        // A tick showing the entity's current angle.
        const vec2 x = screenDir(view, pivot, axisDir(target, 0, true));
        p.line(o, o + x * R, lw * 0.6f, col(GizmoHandle::Rotate, kColRing));
        p.box(o, centre, 0, col(GizmoHandle::MoveFree, kColFree));
        return;
    }
    const bool local = mode == GizmoMode::Scale || localSpace;
    for (int axis = 0; axis < 2; ++axis) {
        const vec2 dirW = axisDir(target, axis, local);
        const vec2 s = screenDir(view, pivot, dirW);
        const GizmoHandle h = mode == GizmoMode::Move ? (axis ? GizmoHandle::MoveY : GizmoHandle::MoveX)
                                                      : (axis ? GizmoHandle::ScaleY : GizmoHandle::ScaleX);
        const uint32_t c = col(h, axis ? kColY : kColX);
        p.line(o, o + s * (L - tip * 0.5f), lw, c);
        const float angle = std::atan2(dirW.y, dirW.x);
        if (mode == GizmoMode::Move) p.box(o + s * L, tip, angle + kPi / 4, c);   // diamond arrowhead
        else p.box(o + s * L, tip, angle, c);                                     // scale box
    }
    p.box(o, centre, 0, col(mode == GizmoMode::Scale ? GizmoHandle::ScaleUniform : GizmoHandle::MoveFree, kColFree));
}

}  // namespace mge::editor
