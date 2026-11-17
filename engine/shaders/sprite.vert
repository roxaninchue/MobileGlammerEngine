#version 450
// Sprite vertex shader (Vulkan). No vertex buffer: the quad corner comes from gl_VertexIndex
// (triangle strip, 4 vertices) and everything else from the 36-byte per-instance record.
// Keep in sync with the GLES copy in gles_renderer.cpp and SpriteInstance in sprite_batch.h.
layout(location = 0) in vec4 iAxes;     // axisX.xy, axisY.xy
layout(location = 1) in vec2 iOrigin;
layout(location = 2) in vec4 iUv;       // u0 v0 (top-left), u1 v1 (bottom-right)
layout(location = 3) in vec4 iColor;

layout(push_constant) uniform PushConstants {
    vec4 view;        // xy = world->ndc scale, zw = offset
    vec4 preRotate;   // column-major 2x2 applied in clip space for swapchain pre-rotation
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
    vec2 q = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 world = iOrigin + iAxes.xy * q.x + iAxes.zw * q.y;
    vec2 ndc = world * pc.view.xy + pc.view.zw;
    ndc.y = -ndc.y;   // Vulkan clip space is y-down; world and GLES are y-up
    gl_Position = vec4(mat2(pc.preRotate.xy, pc.preRotate.zw) * ndc, 0.0, 1.0);
    vUv = vec2(mix(iUv.x, iUv.z, q.x), mix(iUv.w, iUv.y, q.y));
    vColor = iColor;
}
