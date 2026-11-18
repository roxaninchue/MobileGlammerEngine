#pragma once
// Touch input and gestures (M1 #17).
//  - TouchQueue: the Android UI thread pushes raw pointer events, the engine thread drains them
//    once per frame. Fixed capacity, no allocation after construction; runs of Move events for
//    the same pointer coalesce, so a burst of 120 Hz touch samples costs one slot per finger.
//  - GestureRecognizer: tap, double tap, long press, drag (with fling velocity) and two-finger
//    pinch (scale, rotation, pan). Thresholds are in dp, so they feel the same on every density.
//  - InputMap: named actions bound to gestures or to screen regions (virtual buttons and
//    sticks). A pointer that lands on a button or stick is captured by it and hidden from the
//    gesture recognizer, so a thumb on a stick never turns into a pinch with the other thumb.
// Positions are surface pixels, origin top-left, Y down (Android view space); use
// render::ScreenLayout::screenToWorld to get world coordinates. Times are CLOCK_MONOTONIC ns
// (Android's uptimeMillis clock, same as std::chrono::steady_clock).
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <mutex>
#include <vector>
#include "mge/math.h"

namespace mge::input {

constexpr int kMaxPointers = 10;

enum class TouchAction : uint8_t { Down, Move, Up, Cancel };

struct TouchEvent {
    TouchAction action = TouchAction::Move;
    int32_t pointerId = 0;     // Android pointer id; ignored for Cancel (cancels every pointer)
    vec2 pos{};                // surface pixels
    int64_t timeNs = 0;
};

// Multi-producer safe, meant for one producer (UI thread) and one consumer (engine thread).
class TouchQueue {
public:
    explicit TouchQueue(uint32_t capacity = 256);
    void push(const TouchEvent& e);
    // Appends every queued event to `out` in order and empties the queue. Returns the count.
    uint32_t drain(std::vector<TouchEvent>& out);
    uint32_t size() const;
    uint32_t dropped() const;   // events lost to overflow since construction

private:
    void dropOne();

    mutable std::mutex mutex_;
    std::vector<TouchEvent> ring_;
    uint32_t head_ = 0, count_ = 0, dropped_ = 0;
};

enum class GestureType : uint8_t {
    Tap,
    DoubleTap,     // emitted right after the second Tap (taps are never delayed)
    LongPress,     // finger held still; a later move beyond the slop still starts a drag
    DragBegin,
    Drag,
    DragEnd,
    PinchBegin,
    Pinch,
    PinchEnd,
};

struct Gesture {
    GestureType type = GestureType::Tap;
    int32_t pointerId = -1;    // primary pointer (first finger for pinches)
    vec2 pos{};                // current position; pinch: midpoint of the two fingers
    vec2 start{};              // where the gesture started
    vec2 delta{};              // movement since the previous event of this gesture (pinch: midpoint pan)
    vec2 velocity{};           // DragEnd: release velocity in px/s (for flings); else 0
    float scale = 1.0f;        // Pinch: distance ratio since the previous Pinch event
    float totalScale = 1.0f;   // Pinch: distance ratio since PinchBegin
    float rotation = 0.0f;     // Pinch: radians since the previous Pinch event, CCW on screen
    bool cancelled = false;    // DragEnd/PinchEnd caused by a system cancel
    int64_t timeNs = 0;
};

struct GestureConfig {
    float density = 1.0f;          // surface px per dp (DisplayMetrics.density)
    float tapSlopDp = 8.0f;        // movement allowed for tap / long press
    float doubleTapSlopDp = 40.0f; // max distance between the two taps of a double tap
    float pinchSlopDp = 0.0f;      // two-finger movement before Pinch events start (0 = immediately)
    int64_t tapMaxNs = 300'000'000;
    int64_t doubleTapNs = 300'000'000;   // max gap from the first tap's release to the second's
    int64_t longPressNs = 500'000'000;
};

class GestureRecognizer {
public:
    explicit GestureRecognizer(const GestureConfig& cfg = {}) : cfg_(cfg) {}
    void setConfig(const GestureConfig& cfg) { cfg_ = cfg; }
    const GestureConfig& config() const { return cfg_; }

    void process(const TouchEvent& e, std::vector<Gesture>& out);
    // Fires time-based gestures (long press). Call once per frame with the current time.
    void update(int64_t nowNs, std::vector<Gesture>& out);
    void reset();

    int pointerCount() const { return count_; }
    bool isDragging() const { return state_ == State::Dragging; }
    bool isPinching() const { return state_ == State::Pinching; }

private:
    enum class State : uint8_t { Idle, Pressed, LongPressed, Dragging, Pinching, Finished };
    struct Pointer {
        int32_t id;
        vec2 pos, start;
        int64_t downNs;
    };
    int find(int32_t id) const;
    void beginPinch(int64_t t, std::vector<Gesture>& out);
    float slopPx(float dp) const { return dp * cfg_.density; }

    GestureConfig cfg_;
    Pointer ptrs_[kMaxPointers]{};
    int count_ = 0;
    State state_ = State::Idle;
    int32_t primary_ = -1;
    // drag
    vec2 lastPos_{}, velocity_{};
    int64_t lastMoveNs_ = 0;
    // pinch
    int32_t pinchA_ = -1, pinchB_ = -1;
    vec2 pinchStartMid_{}, pinchMid_{};
    float pinchStartDist_ = 1, pinchDist_ = 1, pinchAngle_ = 0;
    bool pinchStarted_ = false;
    // double tap
    vec2 lastTapPos_{};
    int64_t lastTapNs_ = 0;
    bool haveLastTap_ = false;
};

// Region in normalized surface coordinates: (0,0) top-left, (1,1) bottom-right.
struct Region {
    vec2 min{0, 0}, max{1, 1};
    bool contains(vec2 n) const { return n.x >= min.x && n.x <= max.x && n.y >= min.y && n.y <= max.y; }
    static Region full() { return {}; }
};

enum class BindingKind : uint8_t {
    Tap,        // triggered by a tap that lands in the region; value = position (px)
    DoubleTap,  // same, double tap
    LongPress,  // same, long press
    Drag,       // drags that start in the region; held while dragging; value = px moved this frame
    Pinch,      // pinches whose midpoint starts in the region; value.x = scale this frame, value.y = rotation
    Button,     // virtual button: captures a pointer that lands in the region; held while it stays down
    Stick,      // virtual stick: captures like Button; value = offset from the touch-down point
                // divided by stickRadiusDp, clamped to the unit circle, Y up
};

struct Binding {
    BindingKind kind = BindingKind::Tap;
    Region region{};
    float stickRadiusDp = 48.0f;
};

using ActionId = int32_t;
constexpr ActionId kNoAction = -1;

struct ActionState {
    bool held = false;          // Button/Stick/Drag/Pinch: active right now
    bool pressed = false;       // became held this frame, or a Tap/DoubleTap/LongPress fired
    bool released = false;      // stopped being held this frame
    uint32_t count = 0;         // triggers this frame (e.g. two quick taps in one frame)
    vec2 value{};               // see BindingKind
    vec2 pos{};                 // last pointer position that touched this action (px)
};

class InputMap {
public:
    explicit InputMap(const GestureConfig& cfg = {}) : gestures_(cfg) {}

    // Several bindings may share a name: they feed the same action. Returns the action id.
    ActionId bind(const char* name, const Binding& binding);
    ActionId find(const char* name) const;
    const ActionState& action(ActionId id) const;
    const ActionState& action(const char* name) const { return action(find(name)); }

    void setSurfaceSize(uint32_t w, uint32_t h) { surfaceW_ = w; surfaceH_ = h; }
    void setConfig(const GestureConfig& cfg) { gestures_.setConfig(cfg); }

    // One frame: clears per-frame edges, routes `events` (pointer capture, then gestures), then
    // fires time-based gestures at `nowNs`. gestures() holds what the recognizer emitted.
    void update(const std::vector<TouchEvent>& events, int64_t nowNs);
    const std::vector<Gesture>& gestures() const { return frameGestures_; }
    bool captured(int32_t pointerId) const;

private:
    struct Entry { uint32_t nameHash; ActionId action; Binding binding; };
    struct Capture { int32_t pointerId; int entry; vec2 down; };
    vec2 normalized(vec2 px) const;
    void applyGesture(const Gesture& g);
    void beginCapture(const TouchEvent& e, int entry);

    GestureRecognizer gestures_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> actionNames_;
    std::vector<ActionState> actions_;
    std::vector<Capture> captures_;
    std::vector<Gesture> frameGestures_;
    std::vector<int> dragOwners_;     // entries owning the active drag / pinch
    std::vector<int> pinchOwners_;
    uint32_t surfaceW_ = 1, surfaceH_ = 1;
};

}  // namespace mge::input
