#include "mge/editor/editor_viewport.h"
#include <cmath>

namespace mge::editor {
using input::Gesture;
using input::GestureType;

void EditorViewport::recompute() {
    view_ = base_;
    view_.camera = render::Camera2D{base_.camera.center + offset_, base_.camera.halfExtent}.zoomed(zoom_);
    view_.pixelsPerUnit = base_.pixelsPerUnit * zoom_;
}

void EditorViewport::frame(EditorSession& session, const std::vector<input::TouchEvent>& events, int64_t nowNs,
                           float dt, uint32_t surfaceW, uint32_t surfaceH, const render::Insets& insets) {
    const render::ScreenLayout before = view_;
    base_ = render::ScreenLayout::compute(design_, surfaceW, surfaceH, insets);
    recompute();
    moved_ = !(before.surfaceW == view_.surfaceW && before.surfaceH == view_.surfaceH &&
               before.camera.center.x == view_.camera.center.x && before.camera.center.y == view_.camera.center.y &&
               before.camera.halfExtent.x == view_.camera.halfExtent.x);

    frameGestures_.clear();
    for (const input::TouchEvent& e : events) gestures_.process(e, frameGestures_);
    gestures_.update(nowNs, frameGestures_);
    if (!events.empty()) moved_ = true;

    Gizmo& gizmo = session.gizmo();
    gizmo.density = gestures_.config().density;
    for (size_t gi = 0; gi < frameGestures_.size(); ++gi) {
        const Gesture& g = frameGestures_[gi];
        const float worldPerPx = 1.0f / max(view_.pixelsPerUnit, 1e-6f);
        // A drag that starts on a gizmo handle edits the selection instead of panning.
        if (g.type == GestureType::DragBegin && gizmo.begin(session, view_, g.start)) {
            gizmoDrag_ = true;
            gizmo.drag(session, view_, g.pos);
            continue;
        }
        if (gizmoDrag_ && (g.type == GestureType::Drag || g.type == GestureType::DragEnd)) {
            if (g.type == GestureType::Drag) {
                gizmo.drag(session, view_, g.pos);
                continue;
            }
            // A second finger (pinch) or a system cancel aborts the edit; a lift commits it.
            bool pinchNext = false;
            for (size_t k = gi + 1; k < frameGestures_.size(); ++k) pinchNext |= frameGestures_[k].type == GestureType::PinchBegin;
            if (g.cancelled || pinchNext) {
                gizmo.cancel(session);
            } else {
                gizmo.drag(session, view_, g.pos);
                gizmo.end(session);
            }
            gizmoDrag_ = false;
            continue;
        }
        switch (g.type) {
            case GestureType::Tap:
                if (view_.screenInViewport(g.pos)) session.select(session.pick(view_.screenToWorld(g.pos)));
                break;
            case GestureType::DoubleTap:
                resetCamera();
                break;
            case GestureType::DragBegin:
            case GestureType::Drag:
                offset_ -= vec2{g.delta.x, -g.delta.y} * worldPerPx;   // content follows the finger
                fling_ = {};
                break;
            case GestureType::DragEnd:
                offset_ -= vec2{g.delta.x, -g.delta.y} * worldPerPx;
                fling_ = g.cancelled ? vec2{} : -vec2{g.velocity.x, -g.velocity.y} * worldPerPx;
                break;
            case GestureType::Pinch: {
                // Keep the world point under the fingers fixed while zooming, then follow their pan.
                const vec2 anchor = view_.screenToWorld(g.pos - g.delta);
                zoom_ = clamp(zoom_ * g.scale, 0.1f, 20.0f);
                recompute();
                offset_ += anchor - view_.screenToWorld(g.pos);
                fling_ = {};
                break;
            }
            default: break;
        }
        recompute();
    }
    if (lengthSq(fling_) > 1e-4f) {
        offset_ += fling_ * dt;
        fling_ *= std::exp(-4.0f * dt);   // ~0.25 s decay
        if (lengthSq(fling_) <= 1e-4f) fling_ = {};
        moved_ = true;
        recompute();
    }
}

}  // namespace mge::editor
