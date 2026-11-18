# MobileGlammerEngine

Build high-quality 2D and 3D games on your phone, with AI as your co-developer.

MobileGlammerEngine (MGE) is an Android app with a real scene editor and a fully in-house game engine and physics engine. It is designed from the ground up for mobile: high visual quality, lean CPU/GPU/memory/battery usage.

## Pillars
1. **Our engine, our physics.** No Unity, Unreal, Godot, Box2D, Bullet or PhysX. Rendering, ECS, physics, audio and asset pipeline are written in-house.
2. **On-device creation.** A touch-first scene editor: hierarchy, inspector, gizmos, 2D and 3D viewports, play-in-editor.
3. **AI-native.** Describe a game, a level, a mechanic or a script; the AI generates scenes, entities, behaviours and assets that land directly in the editor as editable data.
4. **Lean by design.** Mobile-first tricks (see docs/ARCHITECTURE.md) keep frame time, thermals and APK size down.

## Repository layout (planned)
```
engine/        C++ core (ECS, renderer, physics, audio, assets, scripting VM)
  core/        memory, jobs, math, containers
  render/      Vulkan backend (+ GLES 3 fallback), 2D batcher, 3D forward+ pipeline
  physics/     2D and 3D rigid bodies, collision, broadphase, solver
  scene/       ECS, transforms, serialization
  script/      lightweight game scripting runtime
app/           Android app (Kotlin + Jetpack Compose UI, JNI bridge to engine)
editor/        scene editor tools (shared logic between app and engine)
ai/            AI generation layer (prompt -> scene/entity/script/asset)
tools/         asset cookers, build scripts
docs/          design docs
```

## Shaders
Vulkan SPIR-V is generated ahead of time and committed, so Gradle and CI never need a shader compiler.
After editing anything in `engine/shaders/`, run `tools/compile_shaders.sh` (uses `glslc`, the NDK's
`shader-tools/glslc`, or `glslangValidator`) and commit `engine/src/render/vulkan/generated/sprite_spv.h`.
The GLES 3 copies of the sprite shaders live in `engine/src/render/gles/gles_renderer.cpp`.

## Screen scaling
`mge/render/camera.h` turns a design area (the world rect a game is authored for) plus the surface
size and safe-area insets into a camera and viewport: `Letterbox`, `Expand` (default), `Crop`,
`FitWidth` or `FitHeight`, optionally pixel perfect (whole screen pixels per texel). The app runs
edge to edge and passes system-bar and cutout insets to the engine. Try the modes on the demo:

    adb shell setprop debug.mge.scale letterbox     # letterbox|expand|crop|fitwidth|fitheight
    adb shell setprop debug.mge.pixelperfect 1

## Tilemaps
`mge/render/tilemap.h`: grid tilesets (margin/spacing like Tiled, UV inset against seams), tiles
with flip bits, 16x16 chunks that cache their instance mesh and rebuild only when edited. Each frame
only chunks inside the camera rect are submitted, through the same sprite batches and shaders.

## Touch input
`mge/input.h`: the UI thread forwards MotionEvents (no allocation) into a `TouchQueue` that coalesces
moves; the engine thread drains it each frame. `GestureRecognizer` emits tap, double tap, long press,
drag (with fling velocity) and pinch (scale, rotation, pan), with dp thresholds. `InputMap` binds named
actions to gestures or to screen regions (virtual buttons and sticks capture their finger, so it never
becomes part of a gesture). In the demo, drag pans, pinch zooms around your fingers, double tap resets.

## physics2d
`mge/physics2d/`: shapes (circle, box, convex polygon via hull, mass, AABBs, ray casts), a
self-balancing dynamic AABB tree with fat AABBs and motion prediction, contact manifolds
(circle/circle, polygon/circle, polygon/polygon via SAT plus edge clipping) with stable feature ids
and speculative contacts, and the `World` (`world.h`):
- static, kinematic and dynamic bodies with any number of shapes; collision filters (category,
  mask, group) and sensors; generational body/shape ids
- fixed timestep (`advance(frameDt)` runs whole 1/60 s steps and returns the interpolation alpha;
  frame rate never changes the result, and two runs are bit-identical)
- sequential impulse solver: warm starting by feature id, friction, a 2x2 block solver for
  two-point manifolds (stacks stay square), restitution with a velocity threshold, non-linear
  position correction
- islands and sleeping: resting islands cost nothing; anything touching an awake body wakes its island
- events: contact begin/end, hits (approach speed), sensor begin/end; ray casts, AABB and point queries

`physics_system.h` bridges it to the ECS: `RigidBody2D` component, poses written into `Transform2D`
(interpolated), bodies removed with their entities, and events delivered as `CollisionEvent2D`
records naming the entities.

## Demo scene and benchmark
`mge/demo/demo_scene.h` is the M1 sample: an ASCII-authored level (tilemap + merged static
colliders), a big culled backdrop, a spawner pouring boxes and balls, a ramp, a kinematic platform,
a sensor bin that turns visitors gold, and impact flashes, all drawn from one atlas
(`tools/make_demo_atlas_ktx.py`). Tap drops a burst, long press blasts, drag/pinch/double tap move
the camera. The app shows live stats (fps, frame time p95, CPU, physics, draw calls, sprites,
bodies, memory) and a frame-time graph (green within 16.7 ms, amber up to 2x, red beyond); logcat
gets a full report every 5 s.

    adb shell setprop debug.mge.bench 800    # benchmark run: 800 bodies, 10 s warm-up, 20 s measured
    adb logcat -s MGE | grep Benchmark       # report; JSON in /data/data/com.glammer.mge/files/bench.json
    adb shell setprop debug.mge.overlay 0    # hide the frame-time graph

Off device, `mge_bench [bodies] [seconds] [json]` (built with the tests) runs the same scene headless.

## License
Proprietary, Copyright (c) 2026 Lackson Kapapa, trading as Blazeinchue. All rights reserved. Public for viewing only. See LICENSE.

## Status
M0 and M1 (2D engine) complete; M2 (editor) next. See docs/ROADMAP.md.
