---
status: plan
id: WO-073
title: The world through the C API — entities, components, queries, systems, blobs — and flecs off the boundary
program: providers
priority: P1
rank: 4
size: L
state: todo
depends: [WO-071, WO-072]
contracts: [kit-abi]
touches:
  - include/engine/engine_api.h
  - include/engine/game_module.h
  - scripts/engine_audit.py
source: docs/plans/engine-api.md §9 step 3; engine-architecture.md §3; roadmap §2.8; DR-0028, DR-0029
---
## Why
Kits and providers see flecs today: `game_module.h` hands kits an `ecs_world_t*`, `EngineEntity` is a flecs id, and the Jolt plugin takes `flecs::world`. The ECS is core (DR-0029) and stays flecs inside; what games, kits and providers use is the engine's own world (DR-0028).

## Done when
- [ ] `eng_world.h` implemented over flecs: entities with stable ids, component registration by schema (WO-071), single-entity access, batched queries, systems in phases with `run_after`, the change stream (WO-072), and `EngBlob` storage for variable-length fields
- [ ] `engine_runtime`'s own tests drive it, and one test reads a query's columns from C with no C++ in the test
- [ ] one first-party kit ported to it (`docs/plans/engine-api-draft/examples/kit.c` is the shape), loading with no flecs or engine C++ include
- [ ] an `engine_audit` rule: no flecs include in `include/engine/` or `src/plugins/`, with today's findings as a baseline that only shrinks, mutation-checked
- [ ] the cost of a query batch measured against a direct flecs query on the 50 000-object scene, and recorded

## Contract
Nothing: today's kit ABI keeps loading old kits until `engine-api.md` §11.3 is answered; the C world API is added beside it.
