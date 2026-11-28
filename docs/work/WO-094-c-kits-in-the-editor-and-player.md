---
status: plan
id: WO-094
title: C kits in the editor and engine_player — the manifest loads C modules, and Play is a C world
program: providers
priority: P1
size: L
state: active
depends: [WO-073]
contracts: [kit-abi]
touches:
  - src/runtime/module_loader.h
  - src/capi/eng_capi.cpp
  - src/capi/eng_world.cpp
  - include/eng/eng_module.h
  - samples/combat_c/combat.c
  - src/capi/eng_kits.cpp
  - src/runtime/kit_host.h
  - scripts/gen_eng_client.py
  - include/eng/eng_client.h
  - tests/c_kit_host_test.cpp
source: WO-073 part 3 (2026-11-28); engine-api.md §10d item 4
---
## Why
WO-073 ported the combat kit to the C API (`samples/combat_c/`), but a C kit runs only in a program that links it through `EngEngineDesc.modules`. The editor and `engine_player` load kits from `project.json` as C++ plugins, and the C world exists only behind `engCreateEngine`. So a game made in the editor still uses the C++ CombatKit, and the C kit proves the API without replacing anything.

## Done when
- [x] the manifest loader recognises a C module (an exported `engModuleEntry` returning an `EngModuleDesc`) and loads it beside C++ kits, refusing a version mismatch by name (2026-11-28: part 1; the description is checked by the same function as `EngEngineDesc.modules`)
- [x] the module reaches the engine by name (`get_proc`, engine-api.md §4), not by resolving the host's symbols, so a kit built with only `include/eng/` loads into a host that hides its C++ symbols (2026-11-28: `engGetProc` and the generated `eng_client.h`; the combat module exports only `engModuleEntry` and imports only the C library)
- [x] the editor's Play world and engine_player's world are C worlds for modules: `on_world_start` at each Play, `on_world_stop` at each Stop, hot reload stopping and starting the module (2026-11-28: in place and Snapshot, in `c_kit_host_test`; the editor and engine_player install the loader)
- [ ] the C combat kit replaces the C++ CombatKit in a project, and the FPS and AI kits deal damage through the C components (or the order records why they cannot yet)
- [x] `kit_lifecycle_test` covers a C module: unloaded mid-play and loaded again, the world surviving (2026-11-28: in `c_kit_host_test` instead, which CI runs; `kit_lifecycle_test` needs the CombatKit repository's build, which CI does not have)

## Contract
Nothing: until this lands, a project's `kits` load as C++ plugins only, and a C kit runs only in a program that links it through `EngEngineDesc.modules`. C++ kits keep loading after it, until engine-api.md §11.3 is answered.

## Log
- 2026-11-28 — **Part 1: C modules load from a project's manifest.**
  - **By name.** `engGetProc` (eng_module.h) answers for every C API
    function. `scripts/gen_eng_client.py` generates both sides from the
    headers: `include/eng/eng_client.h`, a stub per function for a module
    built with `ENG_CLIENT`, and `src/capi/eng_procs.inc`, the engine's table.
    `eng_client_current` fails when either is stale.
  - **The loader.** A library exporting `engModuleEntry` is a C module:
    `ModuleLibrary` hands it to a `CModuleFactory` that `KitHost` holds,
    installed by the C API (`engInternalHostCModules`, `src/capi/eng_kits.cpp`),
    because the runtime cannot depend on the C API. Without it a C module is
    refused by name. The editor (now linking `engine_capi`; the edge
    `src/editor -> src/capi` is baselined by design) and engine_player install it.
  - **The world.** One C world per simulation world, shared by the program's
    handle and every C kit, gone when the last lets go. The C kit plugin:
    on_load when loaded (engine NULL), on_world_start at its
    onSimulationStart, on_world_stop at its onSimulationStop, on_unload at
    detach.
  - **What a kit made goes with it.** Systems and queries remember the kit
    whose callback or system created them; at its stop they are destroyed,
    whatever its own on_world_stop did.
  - **The combat kit as a file**: `samples/combat_c/combat_module.c`, combat.c
    unchanged. It exports only `engModuleEntry` and imports only the C library
    (`calloc`, `free`, `memset`).
  - **Tests.** `c_kit_host_test`, 33 checks: refused without the C API; both
    modules loaded from project.json; combat draining damage; Stop and Play
    again; a hot reload; unloaded and loaded mid-play; Play in Snapshot mode, a
    world of its own. Each of these was broken on purpose and caught:
    - not destroying a kit's systems at its stop (a counter that leaves its
      system behind counted twice a step)
    - not recording the kit as the owner during on_world_start
    - `engGetProc` missing one function (the module refuses to load; 14 checks)
    - a C world per holder instead of one shared
    - a stale generated file (`eng_client_current`)
  - **Left: the C++ kits.** The FPS and AI kits deal damage through the C++
    CombatKit's flecs contract, in their own repositories, and a C++ kit has
    no handle to the C world. Moving them is part 2: either the C++ kits get
    the C world (an `EngWorld*` beside the flecs world they are given), or
    they are ported too.

