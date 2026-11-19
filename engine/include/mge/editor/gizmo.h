#pragma once
// Touch gizmos (M2 #23): move, rotate and scale handles for the selection, sized in dp so they
// stay finger-sized at any zoom, with generous hit areas.
//  - Move: X / Y arrows (world axes, or the entity's axes in local space) and a centre square
//    for free moves. Rotate: a ring around the pivot (+ the centre square to move). Scale: X / Y
//    boxes on the entity's own axes and a centre square for uniform scale.
//  - Drags work in world space and are converted into each entity's parent space, so children
//    of rotated or scaled parents follow the finger exactly.
//  - Several selected entities move / rotate together around the primary's pivot; scale
//    applies the same factor to each in place.
//  - Snapping: positions to the grid (only the dragged axis for an axis move), the total angle
//    to `angleDeg` steps, scale values to `scaleStep`.
//  - While dragging, transforms are written straight to the scene (live preview, no history);
//    end() records one undo step with the before/after of every entity; cancel() restores.
// Pure CPU code in mge_core, covered by host tests.
#include <cmath>
#include <cstdint>
#include <vector>
#include "mge/editor/command.h"
#include "mge/render/camera.h"
#include "mge/render/sprite_batch.h"

namespace mge::editor {

class EditorSession;

enum class GizmoMode : uint8_t { Move, Rotate, Scale };
enum class GizmoHandle : uint8_t { None, MoveX, MoveY, MoveFree, Rotate, ScaleX, ScaleY, ScaleUniform };

struct SnapSettings {
    bool enabled = false;
    float grid = 0.5f;        // world units
    float angleDeg = 15.0f;
    float scaleStep = 0.1f;
};

struct GizmoStyle {
    float axisLengthDp = 80.0f;
    float ringRadiusDp = 70.0f;
    float centreSizeDp = 18.0f;
    float tipSizeDp = 14.0f;
    float lineWidthDp = 3.0f;
    float hitRadiusDp = 22.0f;   // finger tolerance around every handle
};

class Gizmo {
public:
    GizmoMode mode = GizmoMode::Move;
    bool localSpace = false;   // move along the entity's own axes instead of the world's
    SnapSettings snap;
    GizmoStyle style;
    float density = 1.0f;      // surface px per dp

    // Handle under a screen point for a gizmo drawn at `pivot` with the entity's world matrix.
    GizmoHandle hitTest(const render::ScreenLayout& view, const scene2d::Affine2& target, vec2 px) const;

    // Starts a drag if `px` lands on a handle of the primary selection. Returns true if the
    // gizmo took the drag (the viewport must not pan then).
    bool begin(EditorSession& session, const render::ScreenLayout& view, vec2 px);
    void drag(EditorSession& session, const render::ScreenLayout& view, vec2 px);
    // Records the whole drag as one undo step ("Move", "Rotate 3"...). No step if nothing moved.
    void end(EditorSession& session);
    // Puts every entity back where the drag started; records nothing.
    void cancel(EditorSession& session);

    bool active() const { return active_ != GizmoHandle::None; }
    GizmoHandle activeHandle() const { return active_; }

    // Handles for the primary selection (screen-sized), highlighted while dragged.
    void draw(render::SpriteBatcher& batcher, const render::ScreenLayout& view, const scene2d::Affine2& target) const;

    // Exposed for tests: the snapped value of `v` for a step (no-op when step <= 0).
    static float snapTo(float v, float step) { return step > 0 ? std::round(v / step) * step : v; }

private:
    struct Start {
        Uid uid;
        std::vector<uint8_t> bytes;   // Transform2D in SceneDoc encoding
        scene2d::Affine2 world;       // entity world matrix
        scene2d::Affine2 parentWorld; // its parent's (identity for roots)
    };
    void apply(EditorSession& session, const scene2d::Affine2& delta, vec2 scaleFactor);
    vec2 axisDir(const scene2d::Affine2& target, int axis, bool local) const;   // world-space unit vector

    GizmoHandle active_ = GizmoHandle::None;
    std::vector<Start> starts_;
    vec2 startPx_{}, startWorld_{}, pivot_{}, startOriginPx_{};
    float startAngle_ = 0, lastAngle_ = 0, totalAngle_ = 0, curAngle_ = 0;
    vec2 axis_{1, 0};          // world direction of the dragged axis
    scene2d::Affine2 target_;  // primary world matrix at drag start
    bool moved_ = false;
};

}  // namespace mge::editor
