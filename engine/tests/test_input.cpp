#include "test_main.h"
#include "mge/input.h"
#include <cmath>
#include <thread>
using namespace mge;
using namespace mge::input;

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
static constexpr int64_t ms(int64_t v) { return v * 1'000'000; }
static TouchEvent ev(TouchAction a, int id, float x, float y, int64_t tMs) { return {a, id, {x, y}, ms(tMs)}; }
static int countType(const std::vector<Gesture>& g, GestureType t) {
    int n = 0;
    for (const Gesture& x : g) n += x.type == t;
    return n;
}
static const Gesture* last(const std::vector<Gesture>& g, GestureType t) {
    for (size_t i = g.size(); i-- > 0;) if (g[i].type == t) return &g[i];
    return nullptr;
}

TEST(touch_queue_coalesces_and_orders) {
    TouchQueue q(8);
    q.push(ev(TouchAction::Down, 0, 1, 1, 0));
    q.push(ev(TouchAction::Down, 1, 5, 5, 1));
    for (int i = 0; i < 50; ++i) {   // two fingers interleaved at 120 Hz: one slot each
        q.push(ev(TouchAction::Move, 0, 1.0f + float(i), 1, 2 + i));
        q.push(ev(TouchAction::Move, 1, 5, 5.0f + float(i), 2 + i));
    }
    CHECK(q.size() == 4);
    q.push(ev(TouchAction::Up, 0, 50, 1, 60));
    q.push(ev(TouchAction::Move, 1, 5, 99, 61));   // after an Up: new slot, order kept
    std::vector<TouchEvent> out;
    CHECK(q.drain(out) == 6 && q.size() == 0);
    CHECK(out[2].pointerId == 0 && near(out[2].pos.x, 50) && out[3].pointerId == 1 && near(out[3].pos.y, 54));
    CHECK(out[4].action == TouchAction::Up && near(out[5].pos.y, 99));
    CHECK(q.dropped() == 0);
}

TEST(touch_queue_overflow_drops_moves_first) {
    TouchQueue q(4);
    q.push(ev(TouchAction::Down, 0, 0, 0, 0));
    q.push(ev(TouchAction::Move, 0, 1, 0, 1));
    q.push(ev(TouchAction::Down, 1, 0, 0, 2));
    q.push(ev(TouchAction::Down, 2, 0, 0, 3));
    q.push(ev(TouchAction::Up, 2, 0, 0, 4));        // full: the Move goes
    std::vector<TouchEvent> out;
    q.drain(out);
    CHECK(q.dropped() == 1 && out.size() == 4);
    for (const TouchEvent& e : out) CHECK(e.action != TouchAction::Move);
    CHECK(out[3].action == TouchAction::Up);
}

TEST(touch_queue_threads) {
    TouchQueue q(64);
    std::thread producer([&] {
        for (int i = 0; i < 2000; ++i) {
            q.push(ev(TouchAction::Down, i % 3, 0, 0, i));
            q.push(ev(TouchAction::Up, i % 3, 0, 0, i));
        }
    });
    std::vector<TouchEvent> out;
    while (out.size() + q.dropped() < 4000) { q.drain(out); std::this_thread::yield(); }
    producer.join();
    q.drain(out);
    CHECK(out.size() + q.dropped() == 4000);
    for (size_t i = 1; i < out.size(); ++i) CHECK(out[i].timeNs >= out[i - 1].timeNs);
}

TEST(gesture_tap_double_tap_and_slop) {
    GestureConfig cfg; cfg.density = 2.0f;   // tap slop = 16 px
    GestureRecognizer r(cfg);
    std::vector<Gesture> g;
    r.process(ev(TouchAction::Down, 3, 100, 100, 0), g);
    r.process(ev(TouchAction::Move, 3, 110, 105, 30), g);   // within slop
    r.process(ev(TouchAction::Up, 3, 110, 105, 80), g);
    CHECK(g.size() == 1 && g[0].type == GestureType::Tap && g[0].pointerId == 3 && near(g[0].pos.x, 110));
    r.process(ev(TouchAction::Down, 4, 130, 110, 200), g);
    r.process(ev(TouchAction::Up, 4, 130, 110, 250), g);
    CHECK(countType(g, GestureType::Tap) == 2 && countType(g, GestureType::DoubleTap) == 1);
    // Third tap does not double again; a slow tap is not a tap at all.
    r.process(ev(TouchAction::Down, 5, 130, 110, 300), g);
    r.process(ev(TouchAction::Up, 5, 130, 110, 340), g);
    CHECK(countType(g, GestureType::DoubleTap) == 1 && countType(g, GestureType::Tap) == 3);
    g.clear();
    r.process(ev(TouchAction::Down, 6, 0, 0, 1000), g);
    r.process(ev(TouchAction::Up, 6, 0, 0, 1400), g);
    CHECK(g.empty());
    // Too far apart for a double tap.
    r.process(ev(TouchAction::Down, 7, 0, 0, 2000), g);
    r.process(ev(TouchAction::Up, 7, 0, 0, 2050), g);
    r.process(ev(TouchAction::Down, 8, 200, 0, 2100), g);
    r.process(ev(TouchAction::Up, 8, 200, 0, 2150), g);
    CHECK(countType(g, GestureType::Tap) == 2 && countType(g, GestureType::DoubleTap) == 0);
}

TEST(gesture_long_press_then_drag) {
    GestureRecognizer r;
    std::vector<Gesture> g;
    r.process(ev(TouchAction::Down, 0, 50, 50, 0), g);
    r.update(ms(499), g);
    CHECK(g.empty());
    r.update(ms(500), g);
    r.update(ms(900), g);   // only once
    CHECK(g.size() == 1 && g[0].type == GestureType::LongPress && near(g[0].pos.x, 50));
    r.process(ev(TouchAction::Move, 0, 80, 50, 950), g);   // drag after the hold
    CHECK(countType(g, GestureType::DragBegin) == 1);
    r.process(ev(TouchAction::Up, 0, 80, 50, 1000), g);
    CHECK(countType(g, GestureType::DragEnd) == 1 && countType(g, GestureType::Tap) == 0);
}

TEST(gesture_drag_deltas_and_fling) {
    GestureRecognizer r;   // density 1, slop 8 px
    std::vector<Gesture> g;
    r.process(ev(TouchAction::Down, 0, 0, 0, 0), g);
    r.process(ev(TouchAction::Move, 0, 5, 0, 8), g);
    CHECK(g.empty() && !r.isDragging());
    for (int i = 1; i <= 10; ++i) r.process(ev(TouchAction::Move, 0, 10.0f * float(i), 0, 8 + 8 * i), g);
    CHECK(r.isDragging());
    const Gesture* b = last(g, GestureType::DragBegin);
    CHECK(b && near(b->delta.x, 10) && near(b->start.x, 0));
    float sum = 0;
    for (const Gesture& x : g) sum += x.delta.x;
    CHECK(near(sum, 100));   // DragBegin + Drag deltas add up to the full motion
    r.process(ev(TouchAction::Up, 0, 100, 0, 90), g);
    const Gesture* e = last(g, GestureType::DragEnd);
    CHECK(e && e->velocity.x > 1000.0f && e->velocity.x < 1300.0f && !e->cancelled);   // ~1250 px/s
    // Holding still before lifting: no fling.
    g.clear();
    r.process(ev(TouchAction::Down, 1, 0, 0, 1000), g);
    r.process(ev(TouchAction::Move, 1, 50, 0, 1010), g);
    r.process(ev(TouchAction::Up, 1, 50, 0, 1300), g);
    CHECK(last(g, GestureType::DragEnd) && near(last(g, GestureType::DragEnd)->velocity.x, 0));
}

TEST(gesture_pinch_scale_rotation_pan) {
    GestureRecognizer r;
    std::vector<Gesture> g;
    r.process(ev(TouchAction::Down, 0, 100, 100, 0), g);
    r.process(ev(TouchAction::Move, 0, 130, 100, 10), g);   // starts a drag...
    r.process(ev(TouchAction::Down, 1, 230, 100, 20), g);   // ...which a second finger ends
    CHECK(countType(g, GestureType::DragEnd) == 1 && countType(g, GestureType::PinchBegin) == 1);
    CHECK(r.isPinching());
    const Gesture* pb = last(g, GestureType::PinchBegin);
    CHECK(near(pb->pos.x, 180) && near(pb->pos.y, 100));
    // Spread to twice the distance.
    r.process(ev(TouchAction::Move, 0, 80, 100, 30), g);
    r.process(ev(TouchAction::Move, 1, 280, 100, 30), g);
    const Gesture* p = last(g, GestureType::Pinch);
    CHECK(near(p->totalScale, 2.0f) && near(p->scale, 200.0f / 150.0f) && near(p->rotation, 0));
    // Rotate finger 1 a quarter turn CCW around finger 0 (screen up = -y).
    r.process(ev(TouchAction::Move, 1, 80, -100, 40), g);
    p = last(g, GestureType::Pinch);
    CHECK(near(p->rotation, kPi * 0.5f) && near(p->totalScale, 2.0f) && near(p->pos.x, 80) && near(p->pos.y, 0));
    float rot = 0, scale = 1;
    for (const Gesture& x : g) if (x.type == GestureType::Pinch) { rot += x.rotation; scale *= x.scale; }
    CHECK(near(rot, kPi * 0.5f) && near(scale, 2.0f));
    r.process(ev(TouchAction::Up, 1, 80, -100, 50), g);
    CHECK(countType(g, GestureType::PinchEnd) == 1 && !r.isPinching());
    // The finger left behind does nothing (no tap, no drag) until all are lifted.
    size_t n = g.size();
    r.process(ev(TouchAction::Move, 0, 300, 300, 60), g);
    r.process(ev(TouchAction::Up, 0, 300, 300, 70), g);
    CHECK(g.size() == n && r.pointerCount() == 0);
}

TEST(gesture_cancel_and_bad_pointers) {
    GestureRecognizer r;
    std::vector<Gesture> g;
    r.process(ev(TouchAction::Move, 9, 1, 1, 0), g);   // never went down
    r.process(ev(TouchAction::Up, 9, 1, 1, 0), g);
    CHECK(g.empty());
    r.process(ev(TouchAction::Down, 0, 0, 0, 0), g);
    r.process(ev(TouchAction::Move, 0, 40, 0, 10), g);
    r.process(ev(TouchAction::Cancel, -1, 0, 0, 20), g);
    const Gesture* e = last(g, GestureType::DragEnd);
    CHECK(e && e->cancelled && r.pointerCount() == 0);
    // Pinch slop: nothing until the fingers really move.
    GestureConfig cfg; cfg.pinchSlopDp = 10;
    GestureRecognizer s(cfg);
    g.clear();
    s.process(ev(TouchAction::Down, 0, 0, 0, 0), g);
    s.process(ev(TouchAction::Down, 1, 100, 0, 0), g);
    s.process(ev(TouchAction::Move, 1, 104, 0, 5), g);
    CHECK(g.empty());
    s.process(ev(TouchAction::Move, 1, 150, 0, 10), g);
    CHECK(countType(g, GestureType::PinchBegin) == 1 && countType(g, GestureType::Pinch) == 1);
    CHECK(near(last(g, GestureType::Pinch)->totalScale, 1.5f));
}

TEST(input_map_buttons_sticks_and_gestures) {
    GestureConfig cfg; cfg.density = 2.0f;
    InputMap map(cfg);
    map.setSurfaceSize(1000, 500);
    const ActionId move = map.bind("move", {BindingKind::Stick, {{0, 0.5f}, {0.3f, 1}}, 50.0f});   // 100 px radius
    const ActionId jump = map.bind("jump", {BindingKind::Button, {{0.8f, 0.5f}, {1, 1}}});
    map.bind("jump", {BindingKind::DoubleTap, Region::full()});
    const ActionId pan = map.bind("pan", {BindingKind::Drag, Region::full()});
    const ActionId zoom = map.bind("zoom", {BindingKind::Pinch, Region::full()});
    const ActionId select = map.bind("select", {BindingKind::Tap, {{0.3f, 0}, {0.8f, 1}}});
    CHECK(map.find("jump") == jump && map.find("nope") == kNoAction && !map.action("nope").held);

    // Thumb on the stick, other thumb on the button: no pinch.
    map.update({ev(TouchAction::Down, 0, 100, 400, 0), ev(TouchAction::Down, 1, 900, 400, 0)}, ms(0));
    CHECK(map.action(move).pressed && map.action(jump).pressed && map.action(jump).held);
    CHECK(map.captured(0) && map.captured(1) && map.gestures().empty());
    map.update({ev(TouchAction::Move, 0, 160, 320, 16)}, ms(16));   // 60 right, 80 up
    CHECK(near(map.action(move).value.x, 0.6f) && near(map.action(move).value.y, 0.8f));
    CHECK(!map.action(jump).pressed && map.action(jump).held);
    map.update({ev(TouchAction::Move, 0, 400, 400, 32), ev(TouchAction::Up, 1, 900, 400, 32)}, ms(32));
    CHECK(near(map.action(move).value.x, 1.0f) && near(map.action(move).value.y, 0.0f));   // clamped
    CHECK(map.action(jump).released && !map.action(jump).held);
    map.update({}, ms(48));
    CHECK(map.action(move).held && near(map.action(move).value.x, 1.0f));   // stick value persists
    map.update({ev(TouchAction::Up, 0, 400, 400, 60)}, ms(60));
    CHECK(map.action(move).released && near(map.action(move).value.x, 0));
    CHECK(!map.captured(0));

    // A tap in the middle selects; a double tap anywhere also jumps.
    map.update({ev(TouchAction::Down, 2, 500, 100, 100), ev(TouchAction::Up, 2, 500, 100, 140)}, ms(140));
    CHECK(map.action(select).pressed && near(map.action(select).pos.x, 500) && !map.action(jump).pressed);
    map.update({ev(TouchAction::Down, 3, 505, 100, 200), ev(TouchAction::Up, 3, 505, 100, 230)}, ms(230));
    CHECK(map.action(jump).pressed && map.action(jump).count == 1 && !map.action(jump).held);

    // Drag outside the capture regions pans, frame by frame.
    map.update({ev(TouchAction::Down, 4, 500, 100, 1000), ev(TouchAction::Move, 4, 540, 100, 1010)}, ms(1010));
    CHECK(map.action(pan).pressed && map.action(pan).held && near(map.action(pan).value.x, 40));
    map.update({ev(TouchAction::Move, 4, 550, 90, 1020)}, ms(1020));
    CHECK(near(map.action(pan).value.x, 10) && near(map.action(pan).value.y, -10) && !map.action(pan).pressed);
    map.update({}, ms(1030));
    CHECK(near(map.action(pan).value.x, 0) && map.action(pan).held);
    // Second finger turns it into a zoom.
    map.update({ev(TouchAction::Down, 5, 650, 90, 1040), ev(TouchAction::Move, 5, 750, 90, 1050)}, ms(1050));
    CHECK(map.action(pan).released && map.action(zoom).pressed && near(map.action(zoom).value.x, 2.0f));
    map.update({}, ms(1060));
    CHECK(map.action(zoom).held && near(map.action(zoom).value.x, 1.0f));
    map.update({ev(TouchAction::Cancel, -1, 0, 0, 1070)}, ms(1070));
    CHECK(map.action(zoom).released && !map.action(zoom).held);
}

TEST(input_map_long_press_fires_from_update) {
    InputMap map;
    map.setSurfaceSize(100, 100);
    const ActionId menu = map.bind("menu", {BindingKind::LongPress, Region::full()});
    map.update({ev(TouchAction::Down, 0, 10, 10, 0)}, ms(0));
    CHECK(!map.action(menu).pressed);
    map.update({}, ms(600));
    CHECK(map.action(menu).pressed && near(map.action(menu).value.x, 10));
    map.update({}, ms(700));
    CHECK(!map.action(menu).pressed);
}
