---
status: plan
id: WO-008
title: game_module.h stops claiming a static-link path that does not exist
program: providers
priority: P1
size: S
state: done
done: 2026-09-30
evidence: include/engine/game_module.h and docs/contracts/kit-abi.md state both shipping paths; checked against engine_player.cpp, engine_build.cpp and samples/minimal_game
contracts: [kit-abi]
touches:
  - include/engine/game_module.h
source: review 2026-09-29 P3 (verified 2026-09-29, worse than reported)
---
## Why
The header says shipped games "link engine::runtime statically and never dlopen anything". No code does that for the default shipping path (see the log: the SDK path does, for plugin classes).

`ENGINE_GAME_MODULE` exports fixed C names (`engineGameModuleCreateV1`), so two statically linked modules would collide at link time. The only way modules are found is `dlsym`/`GetProcAddress`.

## Done when
- [x] the comment states what is true: today modules load only dynamically, and the static path is WO-025
- [x] `docs/contracts/kit-abi.md`'s **Nothing** section says the same

## Contract
Nothing: on a platform without dynamic loading there is no module path at all today. That is written down, and WO-025 is parked until a target needs it.

## Log
- 2026-09-30: **The finding was overstated in both directions, including in
  this order's own Why.** The header said shipped games "never dlopen".
  Reading the shipping code shows two paths:
  - **The default does `dlopen`.** `engine_build` ships `engine_player` plus
    `kits/`, loaded in place through `ModuleLibrary` (`Reload::Never`).
    `engine_player.cpp` says "this path is the one players actually take".
  - **A game that links `engine::runtime` itself** (`samples/minimal_game`)
    registers `IEnginePlugin` classes with `engine.plugins().add(...)` and
    never loads a module.

  So a static path exists, for plugin classes. What does not exist is a
  static path for `ENGINE_GAME_MODULE` modules: fixed exported names collide,
  and nothing registers the table without `dlsym`. The header and the
  contract now say exactly that. WO-025's scope is narrowed to match.
