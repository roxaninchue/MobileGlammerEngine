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

## Editor: undo/redo
`mge/editor/command.h`: every editor edit is a `Command` on a `CommandStack`. Commands name
entities by `EditorId` (`mge/editor/scene_doc.h`), a stable id saved with the scene, so deleting
an entity and undoing it brings back the same id and every other step in the history still finds
it. Built-in commands cover any registered component (set, remove), entity subtrees (spawn,
despawn: the snapshot keeps children and entity references) and groups. A gizmo drag or slider
scrub pushes one command per frame with the same merge key; they fold into one undo step until
the finger lifts (`seal()`). `begin()`/`end()` group a multi-entity edit into one step. History
is capped (256 steps / 8 MiB by default, oldest dropped first), `isDirty()` tracks the last save
across undo, redo and merges, and `lock()` freezes editing while the game plays in the editor.

## Editor: hierarchy and inspector
The app opens the editor (`mge/editor/editor_session.h`) on a small starter scene. Compose panels
float over the viewport: the hierarchy (tap to select, arrow to expand, long press for rename,
add child, duplicate, delete, move up/down, indent/outdent) and the inspector, which lists every
registered component of the selection from the serialization registry, so new components show
up without UI code. Fields edit as text (`FieldHint` picks degrees, `#RRGGBBAA` colours or a
switch); drag a field's label to scrub it, and the whole drag is one undo step. Reparenting keeps
the entity where it is on screen. In the viewport, tap selects (topmost sprite), drag pans,
pinch zooms, double tap resets the view.

The panels never hold scene data: they send one-line commands (`select 3`,
`set 3 Transform2D position 1 2`, `undo`...) through `NativeEngine.editorCommand`, the render
thread applies them, and the panel state comes back as JSON stamped with a revision. The editor
renders on demand: with nothing changing it draws no frames at all. The M1 physics demo is still
there: `adb shell setprop debug.mge.demo 1` (benchmarks select it automatically).

Scene format v2 adds `Vec4`, `I16`, `U16` and `Str32` fields (v1 scenes still load) and the
`Name` component; `SpriteRenderer` is now saved with scenes (tint and UVs; textures come with
the asset browser).

## Editor: touch gizmos
`mge/editor/gizmo.h`: the selection gets finger-sized handles (sized in dp, generous hit areas):
move (X/Y arrows on world or local axes, centre square for a free move), rotate (a ring; turns
add up past 360 degrees) and scale (X/Y boxes on the entity's own axes, centre for uniform).
Drags are solved in world space and written back in each entity's parent space, so a child of a
rotated, scaled parent tracks the finger exactly; a multi-selection moves and rotates around the
primary's pivot. Snapping (toolbar `#`) puts positions on the grid, angles on 15 degree steps and
scales on 0.1 steps (all adjustable: `snap 1 <grid> <angle> <scale>`). The drag previews live
and lands as one undo step when the finger lifts; a second finger cancels it. A drag that does
not start on a handle still pans the view.

## License
Proprietary, Copyright (c) 2026 Lackson Kapapa, trading as Blazeinchue. All rights reserved. Public for viewing only. See LICENSE.

## Status
M0 and M1 (2D engine) complete; M2 (editor) next. See docs/ROADMAP.md.
