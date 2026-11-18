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

## physics2d (in progress)
`mge/physics2d/`: shapes (circle, box, convex polygon via hull, mass, AABBs, ray casts), a
self-balancing dynamic AABB tree with fat AABBs and motion prediction, a broadphase that reports each
new pair once, and contact manifolds (circle/circle, polygon/circle, polygon/polygon via SAT plus
edge clipping) with stable feature ids and speculative contacts. The solver comes in #19.

## License
Proprietary, Copyright (c) 2026 Lackson Kapapa, trading as Blazeinchue. All rights reserved. Public for viewing only. See LICENSE.

## Status
Project kickoff. See docs/ARCHITECTURE.md and docs/ROADMAP.md.
