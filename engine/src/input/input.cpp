#include "mge/input.h"
#include "mge/assert.h"

namespace mge::input {

// ---------------------------------------------------------------- TouchQueue

TouchQueue::TouchQueue(uint32_t capacity) : ring_(capacity ? capacity : 1) {}

void TouchQueue::dropOne() {
    // Prefer losing a Move (positions are refreshed by the next one) over a Down/Up/Cancel.
    const uint32_t cap = uint32_t(ring_.size());
    for (uint32_t i = 0; i < count_; ++i) {
        uint32_t idx = (head_ + i) % cap;
        if (ring_[idx].action == TouchAction::Move) {
            for (uint32_t j = i; j + 1 < count_; ++j)
                ring_[(head_ + j) % cap] = ring_[(head_ + j + 1) % cap];
            --count_;
            ++dropped_;
            return;
        }
    }
    head_ = (head_ + 1) % cap;
    --count_;
    ++dropped_;
}

void TouchQueue::push(const TouchEvent& e) {
    std::lock_guard lock(mutex_);
    const uint32_t cap = uint32_t(ring_.size());
    if (e.action == TouchAction::Move) {
        // Coalesce into a pending Move of the same pointer in the trailing run of Moves.
        for (uint32_t i = count_; i-- > 0;) {
            TouchEvent& q = ring_[(head_ + i) % cap];
            if (q.action != TouchAction::Move) break;
            if (q.pointerId == e.pointerId) { q = e; return; }
        }
    }
    if (count_ == cap) dropOne();
    ring_[(head_ + count_) % cap] = e;
    ++count_;
}

uint32_t TouchQueue::drain(std::vector<TouchEvent>& out) {
    std::lock_guard lock(mutex_);
    const uint32_t cap = uint32_t(ring_.size());
    const uint32_t n = count_;
    for (uint32_t i = 0; i < n; ++i) out.push_back(ring_[(head_ + i) % cap]);
    head_ = 0;
    count_ = 0;
    return n;
}

uint32_t TouchQueue::size() const { std::lock_guard lock(mutex_); return count_; }
uint32_t TouchQueue::dropped() const { std::lock_guard lock(mutex_); return dropped_; }

// ---------------------------------------------------------------- GestureRecognizer

namespace {
float screenAngle(vec2 a, vec2 b) { return std::atan2(-(b.y - a.y), b.x - a.x); }   // CCW, Y down
float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
}  // namespace

int GestureRecognizer::find(int32_t id) const {
    for (int i = 0; i < count_; ++i)
        if (ptrs_[i].id == id) return i;
    return -1;
}

void GestureRecognizer::reset() {
    count_ = 0;
    state_ = State::Idle;
    primary_ = pinchA_ = pinchB_ = -1;
    pinchStarted_ = false;
    haveLastTap_ = false;
}

void GestureRecognizer::beginPinch(int64_t t, std::vector<Gesture>& out) {
    const Pointer& a = ptrs_[find(pinchA_)];
    const Pointer& b = ptrs_[find(pinchB_)];
    pinchMid_ = pinchStartMid_ = (a.pos + b.pos) * 0.5f;
    pinchDist_ = pinchStartDist_ = max(length(b.pos - a.pos), 1.0f);
    pinchAngle_ = screenAngle(a.pos, b.pos);
    pinchStarted_ = cfg_.pinchSlopDp <= 0.0f;
    if (pinchStarted_) {
        Gesture g;
        g.type = GestureType::PinchBegin;
        g.pointerId = pinchA_;
        g.pos = g.start = pinchMid_;
        g.timeNs = t;
        out.push_back(g);
    }
}

void GestureRecognizer::process(const TouchEvent& e, std::vector<Gesture>& out) {
    const int64_t t = e.timeNs;
    if (e.action == TouchAction::Cancel) {
        Gesture g;
        g.cancelled = true;
        g.timeNs = t;
        if (state_ == State::Dragging) {
            const int i = find(primary_);
            g.type = GestureType::DragEnd;
            g.pointerId = primary_;
            g.pos = lastPos_;
            g.start = i >= 0 ? ptrs_[i].start : lastPos_;
            out.push_back(g);
        } else if (state_ == State::Pinching && pinchStarted_) {
            g.type = GestureType::PinchEnd;
            g.pointerId = pinchA_;
            g.pos = pinchMid_;
            g.start = pinchStartMid_;
            g.totalScale = pinchDist_ / pinchStartDist_;
            out.push_back(g);
        }
        reset();
        return;
    }

    if (e.action == TouchAction::Down) {
        if (find(e.pointerId) >= 0) return;            // duplicate down: ignore
        if (count_ == kMaxPointers) return;
        ptrs_[count_++] = {e.pointerId, e.pos, e.pos, t};
        if (count_ == 1) {
            state_ = State::Pressed;
            primary_ = e.pointerId;
            lastPos_ = e.pos;
            velocity_ = {};
            lastMoveNs_ = t;
        } else if (count_ == 2 && state_ != State::Finished) {
            if (state_ == State::Dragging) {
                Gesture g;
                g.type = GestureType::DragEnd;
                g.pointerId = primary_;
                g.pos = lastPos_;
                g.start = ptrs_[find(primary_)].start;
                g.timeNs = t;
                out.push_back(g);
            }
            state_ = State::Pinching;
            pinchA_ = ptrs_[0].id;
            pinchB_ = ptrs_[1].id;
            beginPinch(t, out);
        }
        return;
    }

    const int idx = find(e.pointerId);
    if (idx < 0) return;   // pointer we never saw go down (or beyond kMaxPointers)

    if (e.action == TouchAction::Move) {
        Pointer& p = ptrs_[idx];
        p.pos = e.pos;
        if ((state_ == State::Pressed || state_ == State::LongPressed) && e.pointerId == primary_) {
            if (lengthSq(e.pos - p.start) > slopPx(cfg_.tapSlopDp) * slopPx(cfg_.tapSlopDp)) {
                state_ = State::Dragging;
                Gesture g;
                g.type = GestureType::DragBegin;
                g.pointerId = primary_;
                g.pos = e.pos;
                g.start = p.start;
                g.delta = e.pos - p.start;
                g.timeNs = t;
                out.push_back(g);
                lastPos_ = e.pos;
                lastMoveNs_ = t;
            }
        } else if (state_ == State::Dragging && e.pointerId == primary_) {
            Gesture g;
            g.type = GestureType::Drag;
            g.pointerId = primary_;
            g.pos = e.pos;
            g.start = p.start;
            g.delta = e.pos - lastPos_;
            g.timeNs = t;
            const float dt = float(t - lastMoveNs_) * 1e-9f;
            if (dt > 1e-4f) {
                // Exponential smoothing of the instantaneous velocity (~ the last 3-4 samples).
                vec2 inst = g.delta / dt;
                velocity_ = lerp(velocity_, inst, 0.6f);
            }
            lastPos_ = e.pos;
            lastMoveNs_ = t;
            out.push_back(g);
        } else if (state_ == State::Pinching && (e.pointerId == pinchA_ || e.pointerId == pinchB_)) {
            const Pointer& a = ptrs_[find(pinchA_)];
            const Pointer& b = ptrs_[find(pinchB_)];
            const vec2 mid = (a.pos + b.pos) * 0.5f;
            const float dist = max(length(b.pos - a.pos), 1.0f);
            const float ang = screenAngle(a.pos, b.pos);
            if (!pinchStarted_) {
                const float slop = slopPx(cfg_.pinchSlopDp);
                if (std::fabs(dist - pinchStartDist_) < slop && length(mid - pinchStartMid_) < slop) return;
                pinchStarted_ = true;
                Gesture g;
                g.type = GestureType::PinchBegin;
                g.pointerId = pinchA_;
                g.pos = g.start = pinchStartMid_;
                g.timeNs = t;
                out.push_back(g);
            }
            Gesture g;
            g.type = GestureType::Pinch;
            g.pointerId = pinchA_;
            g.pos = mid;
            g.start = pinchStartMid_;
            g.delta = mid - pinchMid_;
            g.scale = dist / pinchDist_;
            g.totalScale = dist / pinchStartDist_;
            g.rotation = wrapAngle(ang - pinchAngle_);
            g.timeNs = t;
            pinchMid_ = mid;
            pinchDist_ = dist;
            pinchAngle_ = ang;
            out.push_back(g);
        }
        return;
    }

    // Up
    const Pointer p = ptrs_[idx];
    ptrs_[idx] = ptrs_[--count_];
    if (state_ == State::Pressed && e.pointerId == primary_) {
        const bool still = lengthSq(e.pos - p.start) <= slopPx(cfg_.tapSlopDp) * slopPx(cfg_.tapSlopDp);
        if (still && t - p.downNs <= cfg_.tapMaxNs) {
            Gesture g;
            g.type = GestureType::Tap;
            g.pointerId = e.pointerId;
            g.pos = e.pos;
            g.start = p.start;
            g.timeNs = t;
            out.push_back(g);
            const float dts = slopPx(cfg_.doubleTapSlopDp);
            if (haveLastTap_ && p.downNs - lastTapNs_ <= cfg_.doubleTapNs && lengthSq(e.pos - lastTapPos_) <= dts * dts) {
                g.type = GestureType::DoubleTap;
                out.push_back(g);
                haveLastTap_ = false;   // a third tap starts a new pair
            } else {
                haveLastTap_ = true;
                lastTapPos_ = e.pos;
                lastTapNs_ = t;
            }
        }
        state_ = State::Finished;
    } else if (state_ == State::Dragging && e.pointerId == primary_) {
        Gesture g;
        g.type = GestureType::DragEnd;
        g.pointerId = primary_;
        g.pos = e.pos;
        g.start = p.start;
        g.delta = e.pos - lastPos_;
        // A finger that stopped before lifting should not fling.
        g.velocity = (t - lastMoveNs_) > 100'000'000 ? vec2{} : velocity_;
        g.timeNs = t;
        out.push_back(g);
        state_ = State::Finished;
    } else if (state_ == State::Pinching && (e.pointerId == pinchA_ || e.pointerId == pinchB_)) {
        if (pinchStarted_) {
            Gesture g;
            g.type = GestureType::PinchEnd;
            g.pointerId = pinchA_;
            g.pos = pinchMid_;
            g.start = pinchStartMid_;
            g.totalScale = pinchDist_ / pinchStartDist_;
            g.timeNs = t;
            out.push_back(g);
        }
        state_ = State::Finished;   // the remaining finger does nothing until all are lifted
    } else if (state_ == State::LongPressed && e.pointerId == primary_) {
        state_ = State::Finished;
    }
    if (count_ == 0) {
        state_ = State::Idle;
        primary_ = pinchA_ = pinchB_ = -1;
        pinchStarted_ = false;
    }
}

void GestureRecognizer::update(int64_t nowNs, std::vector<Gesture>& out) {
    if (state_ != State::Pressed || count_ != 1) return;
    const Pointer& p = ptrs_[0];
    if (nowNs - p.downNs < cfg_.longPressNs) return;
    state_ = State::LongPressed;
    haveLastTap_ = false;
    Gesture g;
    g.type = GestureType::LongPress;
    g.pointerId = p.id;
    g.pos = p.pos;
    g.start = p.start;
    g.timeNs = nowNs;
    out.push_back(g);
}

// ---------------------------------------------------------------- InputMap

namespace {
uint32_t hashName(const char* s) {   // FNV-1a
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= uint8_t(*s); h *= 16777619u; }
    return h;
}
const ActionState kNoState{};
}  // namespace

ActionId InputMap::bind(const char* name, const Binding& binding) {
    const uint32_t h = hashName(name);
    ActionId id = find(name);
    if (id == kNoAction) {
        id = ActionId(actions_.size());
        actionNames_.push_back(h);
        actions_.push_back({});
    }
    entries_.push_back({h, id, binding});
    return id;
}

ActionId InputMap::find(const char* name) const {
    const uint32_t h = hashName(name);
    for (size_t i = 0; i < actionNames_.size(); ++i)
        if (actionNames_[i] == h) return ActionId(i);
    return kNoAction;
}

const ActionState& InputMap::action(ActionId id) const {
    if (id < 0 || size_t(id) >= actions_.size()) return kNoState;
    return actions_[size_t(id)];
}

bool InputMap::captured(int32_t pointerId) const {
    for (const Capture& c : captures_)
        if (c.pointerId == pointerId) return true;
    return false;
}

vec2 InputMap::normalized(vec2 px) const {
    return {px.x / float(surfaceW_ ? surfaceW_ : 1), px.y / float(surfaceH_ ? surfaceH_ : 1)};
}

void InputMap::beginCapture(const TouchEvent& e, int entry) {
    captures_.push_back({e.pointerId, entry, e.pos});
    ActionState& a = actions_[size_t(entries_[size_t(entry)].action)];
    if (!a.held) a.pressed = true;
    a.held = true;
    ++a.count;
    a.pos = e.pos;
    a.value = {};
}

void InputMap::applyGesture(const Gesture& g) {
    auto fire = [&](BindingKind kind) {
        for (const Entry& en : entries_) {
            if (en.binding.kind != kind || !en.binding.region.contains(normalized(g.pos))) continue;
            ActionState& a = actions_[size_t(en.action)];
            a.pressed = true;
            ++a.count;
            a.pos = g.pos;
            a.value = g.pos;
        }
    };
    switch (g.type) {
    case GestureType::Tap: fire(BindingKind::Tap); break;
    case GestureType::DoubleTap: fire(BindingKind::DoubleTap); break;
    case GestureType::LongPress: fire(BindingKind::LongPress); break;
    case GestureType::DragBegin:
    case GestureType::PinchBegin: {
        const bool drag = g.type == GestureType::DragBegin;
        std::vector<int>& owners = drag ? dragOwners_ : pinchOwners_;
        owners.clear();
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& en = entries_[i];
            if (en.binding.kind != (drag ? BindingKind::Drag : BindingKind::Pinch)) continue;
            if (!en.binding.region.contains(normalized(g.start))) continue;
            owners.push_back(int(i));
            ActionState& a = actions_[size_t(en.action)];
            if (!a.held) a.pressed = true;
            a.held = true;
            ++a.count;
            a.pos = g.pos;
            if (drag) a.value += g.delta;
        }
        break;
    }
    case GestureType::Drag:
        for (int i : dragOwners_) {
            ActionState& a = actions_[size_t(entries_[size_t(i)].action)];
            a.value += g.delta;
            a.pos = g.pos;
        }
        break;
    case GestureType::Pinch:
        for (int i : pinchOwners_) {
            ActionState& a = actions_[size_t(entries_[size_t(i)].action)];
            a.value.x *= g.scale;
            a.value.y += g.rotation;
            a.pos = g.pos;
        }
        break;
    case GestureType::DragEnd:
    case GestureType::PinchEnd: {
        std::vector<int>& owners = g.type == GestureType::DragEnd ? dragOwners_ : pinchOwners_;
        for (int i : owners) {
            ActionState& a = actions_[size_t(entries_[size_t(i)].action)];
            if (g.type == GestureType::DragEnd) a.value += g.delta;
            if (a.held) a.released = true;
            a.held = false;
            a.pos = g.pos;
        }
        owners.clear();
        break;
    }
    }
}

void InputMap::update(const std::vector<TouchEvent>& events, int64_t nowNs) {
    frameGestures_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const BindingKind k = entries_[i].binding.kind;
        ActionState& a = actions_[size_t(entries_[i].action)];
        a.pressed = a.released = false;
        a.count = 0;
        if (k == BindingKind::Pinch) a.value = {1, 0};
        else if (k != BindingKind::Stick) a.value = {};
    }

    const float density = gestures_.config().density;
    size_t gestureStart = 0;
    for (const TouchEvent& e : events) {
        if (e.action == TouchAction::Cancel) {
            for (const Capture& c : captures_) {
                ActionState& a = actions_[size_t(entries_[size_t(c.entry)].action)];
                if (a.held) a.released = true;
                a.held = false;
                a.value = {};
            }
            captures_.clear();
        } else if (e.action == TouchAction::Down) {
            int hit = -1;
            for (size_t i = 0; i < entries_.size() && hit < 0; ++i) {
                const Binding& b = entries_[i].binding;
                if ((b.kind == BindingKind::Button || b.kind == BindingKind::Stick) &&
                    b.region.contains(normalized(e.pos)))
                    hit = int(i);
            }
            if (hit >= 0) { beginCapture(e, hit); continue; }
        } else {
            bool mine = false;
            for (size_t ci = 0; ci < captures_.size(); ++ci) {
                Capture& c = captures_[ci];
                if (c.pointerId != e.pointerId) continue;
                mine = true;
                const Entry& en = entries_[size_t(c.entry)];
                ActionState& a = actions_[size_t(en.action)];
                a.pos = e.pos;
                if (en.binding.kind == BindingKind::Stick) {
                    const float r = max(en.binding.stickRadiusDp * density, 1.0f);
                    vec2 v = (e.pos - c.down) / r;
                    v.y = -v.y;   // screen Y down -> stick Y up
                    const float l = length(v);
                    a.value = l > 1.0f ? v / l : v;
                }
                if (e.action == TouchAction::Up) {
                    captures_.erase(captures_.begin() + long(ci));
                    // Another pointer may still hold the same action.
                    bool still = false;
                    for (const Capture& o : captures_)
                        if (entries_[size_t(o.entry)].action == en.action) still = true;
                    if (!still) {
                        a.held = false;
                        a.released = true;
                        if (en.binding.kind == BindingKind::Stick) a.value = {};
                    }
                }
                break;
            }
            if (mine) continue;
        }
        gestures_.process(e, frameGestures_);
        for (; gestureStart < frameGestures_.size(); ++gestureStart) applyGesture(frameGestures_[gestureStart]);
    }
    gestures_.update(nowNs, frameGestures_);
    for (; gestureStart < frameGestures_.size(); ++gestureStart) applyGesture(frameGestures_[gestureStart]);
}

}  // namespace mge::input
