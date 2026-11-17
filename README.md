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

## License
Proprietary, Copyright (c) 2026 Lackson Kapapa, trading as Blazeinchue. All rights reserved. Public for viewing only. See LICENSE.

## Status
Project kickoff. See docs/ARCHITECTURE.md and docs/ROADMAP.md.
