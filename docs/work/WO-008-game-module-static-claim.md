---
status: plan
id: WO-008
title: game_module.h stops claiming a static-link path that does not exist
program: providers
priority: P1
size: S
state: todo
contracts: [kit-abi]
touches:
  - include/engine/game_module.h
source: review 2026-09-29 P3 (verified 2026-09-29, worse than reported)
---
## Why
The header says shipped games "link engine::runtime statically and never dlopen anything". No code does that.

`ENGINE_GAME_MODULE` exports fixed C names (`engineGameModuleCreateV1`), so two statically linked modules would collide at link time. The only way modules are found is `dlsym`/`GetProcAddress`.

## Done when
- [ ] the comment states what is true: today modules load only dynamically, and the static path is WO-025
- [ ] `docs/contracts/kit-abi.md`'s **Nothing** section says the same

## Contract
Nothing: on a platform without dynamic loading there is no module path at all today. That is written down, and WO-025 is parked until a target needs it.
