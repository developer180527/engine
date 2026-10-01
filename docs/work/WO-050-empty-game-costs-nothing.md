---
status: plan
id: WO-050
title: An empty game costs nothing — capacity grows with content, never reserved for it
program: providers
priority: P1
size: M
state: done
done: 2026-10-01
evidence: empty project 204.7 -> 95.8 MB mapped (physics 78.4 -> 0.01 MB, bgfx heap 86.5 -> 49.1 MB, render targets 72.2 -> 42.2 MB); empty_game_budget_test (2.74 MB tagged, 0.011 ms/tick; an eager physics world turns it red); physics_capacity_test (lazy, rebuild, no spurious events; removing the re-report suppression turns it red); 50k scene unchanged (527 draws, extract 3.7-4.3 ms); 130 tests (all but the 10 fuzz explore campaigns)
touches:
  - src/plugins/jolt_plugin.h
  - src/render/renderer/targets.cpp
  - src/render/renderer/device.cpp
  - src/render/pipeline/programs.cpp
  - src/render/pipeline/shadow_pass.cpp
  - src/render/pipeline/opaque_pass.cpp
  - CMakeLists.txt
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
- [x] physics allocates nothing until the first body, then sizes its capacity and temp allocator to the body count, growing as it grows (WO-048's 2 000-box piles still settle, with overflow still reported)
- [x] the shadow map exists only while a shadow-casting light does; render targets match what is drawn
- [x] an empty project's mapped memory and per-frame CPU are recorded before and after, and pinned by a test or a gate (a budget that turns red), so the next "sized for big games" change is caught
- [x] each remaining fixed reservation over 1 MB is listed with its owner and why it cannot grow lazily

## Not in scope
The renderer's per-frame cost for a 2D game beyond the empty case (sprite batching, 2D pipeline): a separate order once an empty game is cheap.

## Log
- 2026-10-01:
  - **Physics: nothing until content, then growth.** No `PhysicsSystem`, job
    adapter or temp allocator exists until the first body or character.
    - The world is sized from the scene: bodies at the next power of two at
      least twice the count (min 1 024). Contacts are sized from the MOVABLE
      count, 8 constraints and 16 pairs each (2x WO-048's measured pile
      density).
    - Jolt cannot resize a PhysicsSystem, so growth is a rebuild: bodies
      re-added in entity order with velocities and SLEEP STATE, characters
      re-created from stored settings, pending contacts carried by entity.
    - Growth is proactive (on the body and movable counts), because Jolt
      asserts on any update error where asserts are on. The first
      implementation grew on reported overflow and trapped in
      `stress_physics`.
    - The temp allocator now has a malloc fallback, sized ~1 KB per
      constraint.
  - **Collision events across a rebuild.** A touching-pair set drops the
    re-reports of contacts that never ended, and reports the end of any that
    did not come back. `physics_capacity_test` covers both sleeping and awake
    bodies. It found a real bug first: re-adding sleeping bodies as active
    made ten resting boxes "enter" the floor again, because in this Jolt
    sleep reports contacts as ended. Fixed by preserving sleep state.
  - **Rendering.**
    - The shadow map is created on the first shadow-casting light, with a 1x1
      stand-in bound until then.
    - The editor's scene targets are created on the first `renderScene`, not
      in `init()`.
    - bgfx is compiled with `BGFX_CONFIG_MAX_DRAW_CALLS = 16383`: the
      pipeline caps at 4 096 draws and Metal overflows near 8 192, so bgfx's
      65 535-draw tables were unusable.
  - **Measured** on an empty project, `build-prof`, windowed:

    | | before | after |
    |---|---|---|
    | mapped | 204.7 MB | 95.8 MB |
    | Physics heap | 78.4 MB | 0.01 MB |
    | bgfx heap | 86.5 MB | 49.1 MB |
    | render targets | 72.2 MB | 42.2 MB |
    | `Sim.physics` per frame | 0.040 ms | 0.001 ms |
    | `Render` per frame | 0.038 ms | 0.005 ms |

    The 50k real-content scene is unchanged: 527 draws, 125 shadow draws,
    extraction 3.7–4.3 ms, GPU 6.5 ms. Its mapped memory went 562 -> 448 MB.
  - **The gate.** `empty_game_budget_test`: stock plugins, empty simulation,
    every tagged heap under 8 MB together (2.74 MB) and the median tick under
    1 ms (0.011 ms). An eager physics world turns it red. Render targets need
    a GPU and are covered by the windowed numbers above, not by the gate.
  - **What still costs over 1 MB in an empty game, and why:**
    - bgfx's remaining 49 MB: its other compiled-in tables and command
      buffers, which the engine cannot size at runtime. The custom RHI
      replaces them.
    - The backbuffer HDR target and depth, 42 MB at 2560x1440: the frame is
      drawn into them, so they scale with the window, not with content.
    - flecs' initial world (1.5 MB) and miniaudio (1.4 MB): small and fixed.
  - **Found on the way, not fixed (out of scope):** a host with no project
    root resolves `scripts/autorun` against the current directory, so running
    from the engine repo picks up its two dev test scripts. Real games have a
    project root. The budget test now uses an empty one.
  - Decision record DR-0011: capacity grows with content.

