---
status: plan
id: WO-094
title: C kits in the editor and engine_player — the manifest loads C modules, and Play is a C world
program: providers
priority: P1
size: L
state: todo
depends: [WO-073]
contracts: [kit-abi]
touches:
  - src/runtime/module_loader.h
  - src/capi/eng_capi.cpp
  - src/capi/eng_world.cpp
  - include/eng/eng_module.h
  - samples/combat_c/combat.c
source: WO-073 part 3 (2026-11-28); engine-api.md §10d item 4
---
## Why
WO-073 ported the combat kit to the C API (`samples/combat_c/`), but a C kit runs only in a program that links it through `EngEngineDesc.modules`. The editor and `engine_player` load kits from `project.json` as C++ plugins, and the C world exists only behind `engCreateEngine`. So a game made in the editor still uses the C++ CombatKit, and the C kit proves the API without replacing anything.

## Done when
- [ ] the manifest loader recognises a C module (an exported `engModuleEntry` returning an `EngModuleDesc`) and loads it beside C++ kits, refusing a version mismatch by name
- [ ] the module reaches the engine by name (`get_proc`, engine-api.md §4), not by resolving the host's symbols, so a kit built with only `include/eng/` loads into a host that hides its C++ symbols
- [ ] the editor's Play world and engine_player's world are C worlds for modules: `on_world_start` at each Play, `on_world_stop` at each Stop, hot reload stopping and starting the module
- [ ] the C combat kit replaces the C++ CombatKit in a project, and the FPS and AI kits deal damage through the C components (or the order records why they cannot yet)
- [ ] `kit_lifecycle_test` covers a C module: unloaded mid-play and loaded again, the world surviving

## Contract
Nothing: until this lands, a project's `kits` load as C++ plugins only, and a C kit runs only in a program that links it through `EngEngineDesc.modules`. C++ kits keep loading after it, until engine-api.md §11.3 is answered.

## Log
