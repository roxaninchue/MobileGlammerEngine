# Architecture (draft)

## Stack
- **Engine core:** C++20, built with CMake via the Android NDK. No third-party engine or physics code.
- **App/UI:** Kotlin + Jetpack Compose for editor panels; engine renders into a SurfaceView via JNI.
- **Graphics:** Vulkan 1.1 primary, OpenGL ES 3.0 fallback for older devices.
- **Data:** scenes stored as compact binary for runtime, readable JSON for editing and AI generation.

## Engine modules
| Module | Responsibility |
|---|---|
| core | allocators (frame/arena/pool), job system, math (SIMD/NEON), logging |
| scene | data-oriented ECS (archetype storage), transform hierarchy, prefabs, serialization |
| render | render graph, 2D sprite batcher, 3D forward+ with clustered lights, materials, post FX |
| physics2d | rigid bodies, shapes (circle, box, polygon, capsule), SAP broadphase, sequential impulse solver |
| physics3d | rigid bodies, shapes (sphere, box, capsule, convex hull, mesh), BVH broadphase, GJK/EPA, sequential impulse solver, sleeping |
| audio | mixer on AAudio, 3D spatialisation |
| script | sandboxed scripting VM for game logic (AI-friendly, hot-reload in editor) |
| assets | async streaming, ASTC/ETC2 textures, mesh quantisation |

## Lean-usage tricks
- Fixed-timestep physics with interpolated rendering; island sleeping so idle bodies cost nothing.
- Data-oriented ECS, cache-friendly SoA layouts, NEON SIMD math.
- Frame/arena allocators, zero per-frame heap allocations.
- Dynamic resolution scaling and adaptive frame rate tied to thermal state (Android Thermal API / ADPF).
- Tile-based-GPU friendly rendering: minimal render-pass switches, load/store ops tuned, on-chip MSAA.
- GPU instancing, aggressive frustum and occlusion culling, LODs and impostors for 3D.
- Texture compression (ASTC), mesh quantisation, streaming instead of preloading.
- Editor renders on demand (only when something changes); game runs at full rate only in play mode.
- AI calls are batched and cached; generated content is stored as data, not regenerated.

## Editor
- Hierarchy, inspector, asset browser, 2D/3D viewport with touch gizmos (move/rotate/scale), snapping.
- Play / pause / step in editor, undo/redo command stack.
- Prefabs, tilemaps (2D), terrain (3D, later).
- Export: standalone APK/AAB built from the project.

## AI layer
- Natural language -> structured edits (create entity, add component, write script, build level).
- Outputs validated against engine schemas before applying; every AI change is undoable.
- Pluggable model providers (cloud first, on-device small models later).
