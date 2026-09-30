---
status: plan
id: WO-046
title: Four upward includes out of the layer cycle (core, components, render, systems)
program: process
priority: P1
size: S
state: done
done: 2026-10-01
evidence: scripts/audit_baseline.json LAYER-03 drops 5 edges and gains none (52 -> 47); the module graph's only remaining cycle is runtime <-> scene (WO-047); 133 tests, audit and doctor clean
touches:
  - src/core/transform_utils.h
  - src/components/lod_mesh.h
  - src/runtime/jobs/ (now src/core/jobs/)
  - src/core/lod_limit.h
  - src/runtime/world_query_cache.h
source: architecture audit 2026-09-30 (module graph: 8 of 13 modules form one strongly connected cycle)
---
## Why
Every scripted audit rule passes, but the module graph is not a layering. `src/core`, `components`, `render`, `runtime`, `scene`, `systems`, `assets` and `animation` form ONE cycle: any of them can transitively depend on any other, including `core`, the layer everything is meant to stand on. LAYER-03 only stops NEW edges; nothing ever checked that the recorded 52 make a hierarchy.

Five back-edges hold the cycle together, and all five must go (the audit simulated removing each alone: a cycle always remains). Four are small, each one or two headers in the wrong place:

| edge | cause |
|---|---|
| `core -> components` | `core/transform_utils.h` includes `components/prev_transform.h` for `localMatrixLerp` |
| `components -> render` | `components/lod_mesh.h` includes `render/world/lod.h` for one constant, `kMaxLodLevels` |
| `render -> runtime` | `runtime/jobs/jobs.h` (programs.cpp, extract.cpp) and `runtime/world_query_cache.h` (renderer.h): infrastructure living in the orchestration module |
| `systems -> runtime` | the same two headers (animator_system.h) |

The fifth, `scene -> runtime`, is WO-047.

## Done when
- [x] `localMatrixLerp` (and anything else taking a component) moves from `core/transform_utils.h` to `components/transform_hierarchy.h`, beside the other lerp helpers
- [x] `kMaxLodLevels` lives where both `components` and `render/world` may include it (a core constant, like `core/bone_limit.h`), without breaking LAYER-04
- [x] `runtime/jobs` moves to `core/jobs` (it depends on core alone) and `world_query_cache.h` to `components/` (it needs flecs, which core may not include)
- [x] the four edges are gone from `scripts/audit_baseline.json`'s LAYER-03 list, and no new edge replaces them

## Contract
Headers move; no behaviour changes. Kits include neither `runtime/jobs` nor the query cache directly (check `include/engine/`), so the SDK surface is unchanged.

Nothing: the same code, in the layer its dependencies say it belongs to.

## Log
- 2026-10-01: Four moves, no behaviour change:
  - **`runtime/jobs` became `core/jobs`.** It is still compiled into
    `engine_runtime`, so `engine_core` and the cook tools gain no enkiTS.
    LAYER-01 allows enkiTS in core; it forbids bgfx, ImGui and flecs.
  - **`world_query_cache.h` moved to `components/`**, the ECS layer. It
    needs flecs, which core may not include.
  - **`localMatrixLerp` moved to `components/transform_hierarchy.h`.**
    `nlerpQuat` stays in core as pure math.
  - **`kMaxLodLevels` moved to `core/lod_limit.h`.** `render/world/lod.h`
    re-exports it as `rworld::kMaxLodLevels`, so its 11 users are
    unchanged. `entity_serializer.h`, which reached it only through the
    old include, now names the core constant.

  Five edges left the baseline, not four: `audio -> runtime` existed only
  for the job system. None were added, and 35 files changed their include
  paths. Neither header is in `include/` or `samples/`, so kits and the SDK
  surface are unchanged. The bug ledger's `where:` fields (BUG-0003,
  BUG-0056) follow the files.
