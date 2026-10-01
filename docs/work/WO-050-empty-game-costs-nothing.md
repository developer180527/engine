---
status: plan
id: WO-050
title: An empty game costs nothing — capacity grows with content, never reserved for it
program: providers
priority: P1
size: M
state: todo
touches:
  - src/plugins/jolt_plugin.h
  - src/render/renderer/targets.cpp
  - src/render/pipeline/programs.cpp
source: owner directive 2026-10-01; measured the same day
---
## Why
Owner directive (2026-10-01): developers' scripts will be expensive, so the engine must cost close to nothing when it is idle. With no assets and no scripts, only the main loop running, the engine's own cost must be negligible. A minimal 2D game must not carry the engine's 3D performance or memory. The custom RHI will help later, but this cannot wait for it.

Measured on a fresh `engine_project create` project, `build-prof`, windowed `engine_host --frames 600`:

- **CPU is already fine:** all sim and render work is ~0.15 ms a frame; the rest of `Renderer.frame` (7.9 ms) is the 120 Hz vsync wait.
- **Memory is not:** 205 MB mapped while running.
  - Physics, 78 MB live: Jolt's 64 MB temp allocator and the 65 536-body / 20 480-constraint capacity (WO-048's sizing for game scenes), allocated at startup with zero bodies.
  - Rendering, 86 MB live (72 MB render targets): HDR targets and the 2048² shadow map, created with nothing casting a shadow.
  - Audio, jobs, ECS and core: ~5 MB.

The pattern is reservation for content that does not exist. The fix is growth with content, not smaller fixed numbers: a big scene must still get WO-048's capacity.

## Done when
- [ ] physics allocates nothing until the first body, then sizes its capacity and temp allocator to the body count, growing as it grows (WO-048's 2 000-box piles still settle, with overflow still reported)
- [ ] the shadow map exists only while a shadow-casting light does; render targets match what is drawn
- [ ] an empty project's mapped memory and per-frame CPU are recorded before and after, and pinned by a test or a gate (a budget that turns red), so the next "sized for big games" change is caught
- [ ] each remaining fixed reservation over 1 MB is listed with its owner and why it cannot grow lazily

## Not in scope
The renderer's per-frame cost for a 2D game beyond the empty case (sprite batching, 2D pipeline): a separate order once an empty game is cheap.
