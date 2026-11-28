---
status: plan
id: WO-095
title: The C++ wrapper over the C world API — RAII handles, Result, typed components — so C++ kits and providers stop reaching into flecs
program: providers
priority: P1
size: M
state: todo
depends: [WO-073]
touches:
  - include/eng/
source: review of WO-072..094 against the plan, 2026-11-28 (engine-api.md §8: "C++: a header-only wrapper: RAII handles, Result, typed components"; DR-0028: one C API with a C++ wrapper, for everyone)
---
## Why
The plan has one API for everyone, C, with C++ as a thin header-only wrapper over it (DR-0028, engine-api.md §8). WO-073 built the C world and WO-094 loads C kits, but no order builds the wrapper. Without it, every C++ user of the world has two bad choices: the raw C calls with hand-built descriptors and sizes, or flecs through the old kit ABI. WO-094's open item is exactly that: the FPS and AI kits are C++ and reach the combat kit's components through flecs, and its log weighs giving C++ kits an `EngWorld*` against porting them. The plan's answer is this wrapper, and WO-074 (physics on the world API, next on the board) needs it too, since Jolt's plugin is C++.

## Done when
- [ ] `include/eng/eng.hpp`, header-only over `eng_world.h` and nothing else: `eng::World`, `eng::Entity`, `eng::Query<Ts...>` and `eng::System` as RAII handles; `eng::Result<T>` carrying `EngResult` and the error text; typed components (`world.component<T>("ns.Name")` builds the field list from a description the type gives, checked against `sizeof`/`alignof`)
- [ ] a typed query iterates batches as spans of `T` per term, with WRITE and WRITE_MARKED as distinct types, so marking cannot be forgotten
- [ ] the combat kit's C++ users can be written against it: a test kit in C++ reads and writes `combat.Health` through the wrapper, loaded like any C module, with no flecs or engine C++ include
- [ ] it adds no exported symbol and no cost: the wrapper's query loop measured against the C one on `eng_query_bench`
- [ ] an `engine_audit` rule: `include/eng/eng.hpp` includes only `include/eng/`

## Contract
Nothing: the C API is unchanged; the wrapper is headers over it.

## Not in scope
Porting the FPS and AI kits (WO-094 part 2 decides that, on this); the Python, Swift, JS and Rust layers (engine-api.md §8).
