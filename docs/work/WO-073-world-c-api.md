---
status: plan
id: WO-073
title: The world through the C API — entities, components, queries, systems, blobs — and flecs off the boundary
program: providers
priority: P1
rank: 5
size: L
state: active
depends: [WO-071, WO-072]
contracts: [kit-abi]
touches:
  - include/eng/eng_world.h
  - src/capi/eng_world.cpp
  - src/scene/dynamic_schema.h
  - include/engine/engine_api.h
  - include/engine/game_module.h
  - scripts/engine_audit.py
source: docs/plans/engine-api.md §9 step 3; engine-architecture.md §3; roadmap §2.8; DR-0028, DR-0029
---
## Why
Kits and providers see flecs today: `game_module.h` hands kits an `ecs_world_t*`, `EngineEntity` is a flecs id, and the Jolt plugin takes `flecs::world`. The ECS is core (DR-0029) and stays flecs inside; what games, kits and providers use is the engine's own world (DR-0028).

## Done when
- [ ] `eng_world.h` implemented over flecs: entities with stable ids, component registration by schema (WO-071), single-entity access, batched queries, systems in phases with `run_after`, the change stream (WO-072), and `EngBlob` storage for variable-length fields (2026-11-28: all but systems and blobs, see the log)
- [x] `engine_runtime`'s own tests drive it, and one test reads a query's columns from C with no C++ in the test (2026-11-28: `eng_world_test.c`, plain C11 with only `<eng/engine.h>`, 38 checks; `eng_world_scene_test` for what C cannot see)
- [ ] one first-party kit ported to it (`docs/plans/engine-api-draft/examples/kit.c` is the shape), loading with no flecs or engine C++ include
- [ ] an `engine_audit` rule: no flecs include in `include/engine/` or `src/plugins/`, with today's findings as a baseline that only shrinks, mutation-checked
- [x] the cost of a query batch measured against a direct flecs query on the 50 000-object scene, and recorded (2026-11-28: 0.45 against 0.44 ns per entity; `eng_query_bench`, see the log)

## Contract
Nothing: today's kit ABI keeps loading old kits until `engine-api.md` §11.3 is answered; the C world API is added beside it.

## Log
- 2026-11-28 — **Part 1 of 3: the world, entities, components, queries and
  the change stream.** Done in parts because the kit port also needs the
  module system (`eng_module.h`), which does not exist yet:
  1. this part: `eng_world.h` but systems and blobs; the C test; the
     measurement;
  2. systems in phases with `run_after`, and `EngBlob` storage;
  3. modules (`EngEngineDesc.modules`, `on_world_start`), one first-party kit
     ported, and the audit rule.

  What is in:
  - `include/eng/eng_world.h`, implemented in `src/capi/eng_world.cpp`.
    - One world, the engine's simulation world. A second handle held at once,
      or a step other than 1/60, is refused by name.
    - An `EngEntity` is the flecs id, generation included, so a destroyed
      one is refused.
    - Names use the engine's `Name`; stable ids use `EntityId`.
    - Parenting goes through `safeReparent`, which refuses cycles and chains
      that are too deep.
  - **Components by schema.** Size, alignment and a field list, checked whole:
    known types, inside the component, no overlap, distinct names.
    - Registering the same schema again returns the same id. Any difference
      is refused, naming both sides.
    - `core.*` is the engine's namespace.
    - ASSET and BLOB fields, and `migrate`, are refused by name in this
      version.
    - Each component gets a scene schema from its fields
      (`scene/dynamic_schema.h`): saved and loaded under its name, an entity
      field as the target's stable id, linked again on load.
    - Each is also classified as simulation state, hashed field by field. The
      sim hash gained a hash function with a context
      (`declareIdWithContext`), because one function serves every C layout.
  - **One-entity access** copies bytes of the registered size, through
    `ecs_set_id`, so the change stream sees every set.
  - **Queries.** READ, WRITE, WITH, WITHOUT and WRITE_MARKED terms, self
    only, so every column is an array.
    - Structural changes between Begin and End are deferred to End.
    - WRITE marks the batch's rows; `engMarkChanged` marks only the rows
      given.
    - A query handle carries a generation; destroying a query with an open
      iteration is refused.
  - **The change stream:** subscribe, read and unsubscribe over
    `components/change_stream.h`.
  - **Tests.**
    - `eng_world_test.c`, plain C11 with only `<eng/engine.h>`, 38 checks.
    - `eng_world_scene_test`: a C component's fields saved and loaded, an
      entity field linked again, an unreadable value keeping the default, and
      the sim hash covering it and moving with a field.
    - `eng_header_check` (strict C99, no padding) and `eng_exports` now
      include `eng_world.h`.
  - **Measured** (`eng_query_bench`, release, this Mac): 50 000 entities with
    `core.Transform` and a kit component over 8 tables, one pass reading one
    and writing the other, median of 51.

    | | per pass | per entity |
    |---|---|---|
    | flecs, the same terms directly | 0.022 ms | 0.44 ns |
    | C API | 0.023 ms | 0.45 ns |
    | C API, a change stream watching the written column | 1.6 ms | 32 ns |

    The C API costs nothing measurable. The watched case is the stream's own
    per-entity merge (WO-072's hash map), worth making cheaper before a
    provider watches a column that large every frame.

