#pragma once
// Editor viewport (M2 #22): the scene view's camera and touch handling.
//  - Tap picks the topmost sprite under the finger (tap on nothing clears the selection).
//  - Drag pans (content follows the finger, with fling), pinch zooms around the fingers,
//    double tap resets the camera.
//  - Tracks whether anything moved, so the host can render on demand: an idle editor draws
//    nothing (lean by default, see docs/ARCHITECTURE.md).
// Pure CPU code in mge_core, covered by host tests.
#include <cstdint>
#include <vector>
#include "mge/editor/editor_session.h"
#include "mge/input.h"
#include "mge/render/camera.h"

namespace mge::editor {

class EditorViewport {
public:
    explicit EditorViewport(const render::DesignSpec& design = {}) : design_(design) {}

    void setGestureConfig(const input::GestureConfig& cfg) { gestures_.setConfig(cfg); }
    const input::GestureConfig& gestureConfig() const { return gestures_.config(); }

    // One frame: layout for the surface, then the touch events since the last frame, then the
    // camera fling. Changes the session's selection on taps.
    void frame(EditorSession& session, const std::vector<input::TouchEvent>& events, int64_t nowNs, float dt,
               uint32_t surfaceW, uint32_t surfaceH, const render::Insets& insets = {});

    const render::ScreenLayout& layout() const { return view_; }
    vec2 screenToWorld(vec2 px) const { return view_.screenToWorld(px); }
    // True when the last frame changed what is on screen (camera moved, input arrived) or a
    // fling is still running.
    bool needsRedraw() const { return moved_ || lengthSq(fling_) > 1e-4f; }
    // A finger is down: keep ticking frames so time-based gestures (long press) can fire.
    bool touching() const { return gestures_.pointerCount() > 0; }
    // Gestures recognized during the last frame (the gizmo layer reads them too).
    const std::vector<input::Gesture>& gestures() const { return frameGestures_; }

    void resetCamera() { offset_ = {}; zoom_ = 1.0f; fling_ = {}; moved_ = true; }
    vec2 offset() const { return offset_; }
    float zoom() const { return zoom_; }
    render::DesignSpec& design() { return design_; }

private:
    void recompute();

    render::DesignSpec design_;
    render::ScreenLayout base_, view_;
    input::GestureRecognizer gestures_;
    std::vector<input::Gesture> frameGestures_;
    vec2 offset_{};
    float zoom_ = 1.0f;
    vec2 fling_{};
    bool moved_ = true;
};

}  // namespace mge::editor
