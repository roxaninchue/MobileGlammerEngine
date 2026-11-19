# Roadmap

## M0 — Foundations
- [x] CMake + NDK build, Android app shell, JNI bridge
- [x] Core: math, allocators, job system, logging
- [x] Vulkan bootstrap with GLES fallback, clear-screen on device

## M1 — 2D engine
- [x] ECS + transforms + serialization
- [x] Sprite batcher, cameras, tilemaps
- [x] physics2d: shapes, broadphase, solver, collision events

## M2 — Editor v1
- [x] Hierarchy, inspector, touch gizmos, undo/redo
- [x] Play-in-editor, save/load projects

## M3 — 3D engine
- [ ] Mesh/material pipeline, forward+ lighting, shadows
- [ ] physics3d: GJK/EPA, BVH, solver, sleeping

## M4 — AI creation
- [ ] Prompt -> scene/entity/script edits with validation and undo
- [ ] AI-assisted asset generation

## M5 — Ship it
- [ ] Export games as APK/AAB
- [ ] Performance budget tooling (frame time, thermals, memory)
