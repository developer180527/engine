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
  - include/eng/eng_module.h
  - samples/combat_c/combat.c
source: docs/plans/engine-api.md §9 step 3; engine-architecture.md §3; roadmap §2.8; DR-0028, DR-0029
---
## Why
Kits and providers see flecs today: `game_module.h` hands kits an `ecs_world_t*`, `EngineEntity` is a flecs id, and the Jolt plugin takes `flecs::world`. The ECS is core (DR-0029) and stays flecs inside; what games, kits and providers use is the engine's own world (DR-0028).

## Done when
- [x] `eng_world.h` implemented over flecs: entities with stable ids, component registration by schema (WO-071), single-entity access, batched queries, systems in phases with `run_after`, the change stream (WO-072), and `EngBlob` storage for variable-length fields (2026-11-28: parts 1 and 2, see the log; `parallel` systems are refused by name)
- [x] `engine_runtime`'s own tests drive it, and one test reads a query's columns from C with no C++ in the test (2026-11-28: `eng_world_test.c`, plain C11 with only `<eng/engine.h>`, 38 checks; `eng_world_scene_test` for what C cannot see)
- [x] one first-party kit ported to it (`docs/plans/engine-api-draft/examples/kit.c` is the shape), loading with no flecs or engine C++ include (2026-11-28: the combat kit, `samples/combat_c/`, linked in through `EngEngineDesc.modules`; built with only `include/` on its path. Loading it in the editor and engine_player is WO-094)
- [x] an `engine_audit` rule: no flecs include in `include/engine/` or `src/plugins/`, with today's findings as a baseline that only shrinks, mutation-checked (2026-11-28: KIT-01, followed through our own headers, also over `include/eng/` and the C kit; 12 baselined)
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
- 2026-11-28 — **Part 2 of 3: systems and blobs.**
  - **Systems** (`engCreateSystem`, `engDestroySystem`).
    - Three phases:
      - FIXED_UPDATE runs once per simulation step, at exactly 1/60, where
        kits' `onUpdate` runs: before physics, inside the command window and
        the authority check.
      - UPDATE runs once per frame, before every kit's `onFrame`.
      - LATE_UPDATE runs once per frame, after all of them.
    - They hang on two new **edges** of the plugin registry (`setEdges`),
      broadcast before and after every plugin whatever order kits load or
      hot-reload in. Without a C world the edges are null and nothing changes.
    - **Order within a phase.** Systems run in creation order, moved only as
      `run_after` requires. A name not created yet counts once it is.
    - **Refused by name:** a cycle (the message prints it,
      `t.y -> t.x -> t.y`), a name in another phase (whichever system comes
      second), a name taken, a name not namespaced, running after itself.
    - **`parallel` is refused, not ignored.** Batches on the pool need the
      change stream's marks to be thread-safe, and they are not.
    - **With a query** the system is called once per batch, and its context
      carries the iteration for `engMarkChanged`. A query a live system runs
      cannot be destroyed.
    - **From inside a system,** a new system runs from the phase's next run,
      and a destroyed one does not run again. `engDestroyWorld` from inside a
      system is refused.
  - **Blobs** (`engCreateBlob`, `engGetBlob`, `engSetBlob`; `ENG_FIELD_BLOB`
    is no longer refused).
    - **Owned by the field that holds them.** Storing a handle takes the blob.
      Storing a handle another field holds stores a copy, so two fields never
      share bytes. A blob is freed when its field lets go: set to another
      handle or 0, the component removed, or the entity destroyed.
    - **One rule in one place** (`BlobStore::reconcile`). It runs on OnSet
      (add, set, scene load) and on every row a query's WRITE column handed
      out, because a write in place fires no OnSet. It costs nothing for a
      component without blob fields.
    - **Handles carry a generation,** so a freed one is refused and never
      reused.
    - **`engSetBlob` counts as a change** of the component holding the blob
      (the change stream reports it).
    - **Saved** as base64 (`scene/dynamic_schema.h`) and loaded into a blob of
      its own. Text that is not base64 keeps the default.
    - **Hashed by its bytes,** never its handle.
    - The store is the world's, found through a `DynamicBlobsRef` singleton
      (exempt from the hash), because scene schemas are the process's.
    - At most 16 MiB a blob, a BLOB field is one handle, and its default must
      be 0.
  - **Tests.**
    - `eng_systems_test.c`, plain C11, 64 checks. Each of these was broken on
      purpose and caught:
      - the run_after edges
      - copying a held blob
      - settling a WRITE batch
      - freeing on remove
      - the order of the frame's phases
      - the change mark from `engSetBlob`
      - the handle generation (this one first slipped past a check that did
        not compare against the slot just freed; tightened)
    - `eng_world_scene_test`: the blob saved as base64, loaded into its own
      blob, bad text keeping the default, and the hash reading bytes, not
      handles (also broken on purpose and caught).
    - `eng_header_check` covers the two new structs.
- 2026-11-28 — **Part 3 of 3: modules, the combat kit in C, and the audit
  rule.**
  - **Modules** (`include/eng/eng_module.h`, `EngEngineDesc.modules`).
    - Four optional callbacks:
      - `on_load` at `engCreateEngine`, in list order;
      - `on_world_start` at `engCreateWorld`, in list order;
      - `on_world_stop` at `engDestroyWorld` (or `engDestroyEngine` with a
        world held), in reverse;
      - `on_unload` at `engDestroyEngine`, in reverse.
    - Each callback gets back the state its module's `on_load` wrote.
    - **A failing `on_load` or `on_world_start`** fails the call. The modules
      before it are undone in reverse, and the error names the module and
      carries the message its own calls left.
    - **No world is created or destroyed from a callback;** refused by name.
    - **Refused by name:** a NULL entry (by index), a name twice, no name, a
      wrong `api_version`, `reserved` not zero, and services (not in this
      version).
  - **The combat kit in C** (`samples/combat_c/`). The C++ CombatKit lives in
    its own repository (`kits/CombatKit`, ignored here), which CI never
    builds, so the port lives in this repo. Same rule as the C++ kit: damage
    in an inbox, drained once per step into Health, only entities with Health
    hurt, Died added once and the entity left for the game.
    - `combat.h` is the dealer's contract: three layouts, their names, and
      `combatDealDamage`.
    - Built as its own library with only `include/` on its path and nothing
      linked, so a flecs or engine C++ include fails to compile (tried).
    - The C++ kit stays for the editor, the FPS and AI kits; WO-094 moves them.
  - **KIT-01** in `engine_audit`: what `include/engine/`, `include/eng/`,
    `src/plugins/` and the C kit include reaches no flecs, followed through our
    own headers (quoted includes looked for beside the file first, as the
    compiler does). 12 today, baselined:
    - all five plugin headers, through `runtime/plugin.h`;
    - `stock_providers.cpp`, through `runtime/runtime.h`;
    - `include/engine/`'s `engine.h`, `plugin.h`, `plugins.h`, `components.h`
      (through `collision_events.h`) and `game_module.h`.
  - **Tests.** `eng_module_test.c`, plain C11, 46 checks. Each of these was
    broken on purpose and caught:
    - stopping in list order (3 failures)
    - not undoing a failed `on_world_start` (1)
    - the kit not draining the inbox (5)
    - for KIT-01: `<flecs.h>` in `combat.h`, and `eng_module.h` reaching it
      through `engine/plugin.h`; an include in a comment is not one.

    Removing the kit's "died already" check was not caught, and cannot be:
    `engAddComponent` refuses a component the entity has, so `Died` is added
    once either way. `eng_step_test`'s check that modules are refused now
    checks a count with no list.

