#pragma once
// Cameras and screen scaling (M1 #15).
//  - Camera2D: orthographic, Y up, world -> clip = (world - center) / halfExtent.
//  - ScreenLayout: turns a design area (the world rect the game is authored for) plus the
//    surface size and safe-area insets into a camera and a pixel viewport, for one of
//    several scaling policies, optionally pixel-perfect (integer screen pixels per art pixel).
//  - Helpers the backends share: the clip-space view transform for a sub-viewport, and the
//    rect rotation Vulkan needs for scissors under swapchain pre-rotation.
// Pure CPU code in mge_core, covered by host tests. Culling happens in SpriteBatcher against
// Camera2D::bounds(), which is exactly what the viewport shows.
#include <cstdint>
#include "mge/math.h"

namespace mge::render {

// Axis-aligned world rectangle.
struct Rect {
    vec2 min, max;
    bool overlaps(const Rect& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y;
    }
    bool contains(vec2 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
    vec2 size() const { return max - min; }
    vec2 center() const { return (min + max) * 0.5f; }
};

// Pixel rectangle in the app's orientation, origin top-left, Y down (Android view space).
// w == 0 or h == 0 means "the whole surface".
struct Viewport {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    bool full() const { return w == 0 || h == 0; }
    bool operator==(const Viewport& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// Areas covered by system bars, cutouts and rounded corners, in pixels.
struct Insets {
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

struct Camera2D {
    vec2 center{};
    vec2 halfExtent{1, 1};           // world units from center to each viewport edge
    Rect bounds() const { return {center - halfExtent, center + halfExtent}; }
    vec2 scale() const { return {1.0f / halfExtent.x, 1.0f / halfExtent.y}; }

    // Fit `visibleHeight` world units vertically, width follows the viewport aspect.
    static Camera2D fitHeight(vec2 center, float visibleHeight, uint32_t viewportW, uint32_t viewportH);

    // Zoom > 1 magnifies (shows less world).
    Camera2D zoomed(float zoom) const { return {center, halfExtent / zoom}; }

    // Moves center so the viewport's left/bottom edges fall on the screen pixel grid for the
    // given screen pixels per world unit. Call every frame for a moving pixel-art camera.
    Camera2D snapped(float pixelsPerUnit) const;

    // Viewport-local pixels (top-left origin, Y down) <-> world.
    vec2 viewportToWorld(vec2 px, uint32_t viewportW, uint32_t viewportH) const;
    vec2 worldToViewport(vec2 world, uint32_t viewportW, uint32_t viewportH) const;
};

enum class ScaleMode : uint8_t {
    Letterbox,   // whole design area visible at max size inside the safe area; bars elsewhere
    Expand,      // whole design area inside the safe area, extra world fills the rest of the screen
    Crop,        // design area covers the whole screen; edges may be cut off (and land under insets)
    FitWidth,    // design width spans the safe width; height follows the aspect
    FitHeight,   // design height spans the safe height; width follows the aspect
};

struct DesignSpec {
    Rect area{{-8, -4.5f}, {8, 4.5f}};   // world rect the game is authored for
    ScaleMode mode = ScaleMode::Expand;
    // Pixel perfect: screen pixels per art pixel is a whole number (>= 1). `artPixelsPerUnit`
    // says how many texels make one world unit. The scale is rounded down, so the design area
    // still fits; the camera is snapped to the pixel grid.
    bool pixelPerfect = false;
    float artPixelsPerUnit = 16.0f;
};

struct ScreenLayout {
    Viewport viewport;          // where the camera draws, in surface pixels; may be full()
    Camera2D camera;            // maps exactly onto `viewport`
    Rect safeWorld;             // world rect that is on screen and outside every inset
    float pixelsPerUnit = 1;    // screen pixels per world unit
    uint32_t pixelScale = 0;    // screen pixels per art pixel when pixel perfect, else 0
    uint32_t surfaceW = 0, surfaceH = 0;

    static ScreenLayout compute(const DesignSpec& design, uint32_t surfaceW, uint32_t surfaceH,
                                const Insets& insets = {});

    // Surface pixels (top-left origin, Y down), e.g. touch positions, <-> world.
    vec2 screenToWorld(vec2 px) const;
    vec2 worldToScreen(vec2 world) const;
    bool screenInViewport(vec2 px) const;
};

// The 4 floats backends feed the sprite shader (clip = world * xy + zw, Y up) for a camera
// drawn into `vp` on a `surfaceW` x `surfaceH` surface. The viewport is folded into the
// transform, so the GPU viewport stays the whole surface and pre-rotation is untouched.
vec4 viewTransform(const Camera2D& camera, const Viewport& vp, uint32_t surfaceW, uint32_t surfaceH);

// Rotates an app-orientation pixel rect into the swapchain's native orientation.
// quarterTurns is the surface transform's CCW rotation in 90 degree steps (0..3), matching
// the pre-rotation 2x2 the Vulkan backend applies in clip space (after its Y flip).
// appW/appH are the app-orientation surface size.
Viewport rotateToNative(const Viewport& r, uint32_t appW, uint32_t appH, int quarterTurns);

}  // namespace mge::render
