---
status: plan
id: WO-025
title: Static module registration for platforms without dlopen
program: providers
priority: P3
size: M
state: parked
parked-until: a console or iOS target is real
depends: [WO-008]
contracts: [kit-abi]
touches:
  - include/engine/game_module.h
  - src/runtime/module_loader.h
source: review 2026-09-29 P3
---
## Why
Consoles and iOS can't `dlopen`. Modules must be linked in and registered by a table.

## Done when
- [ ] `ENGINE_GAME_MODULE` has a static form with per-module symbol names plus a registration table
- [ ] the loader accepts either source; a test links two modules statically into one binary

## Contract
Nothing: on a no-dlopen platform the loader finds zero dynamic modules and reports it once. It never fails silently.
