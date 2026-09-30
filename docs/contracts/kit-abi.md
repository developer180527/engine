---
status: as-built
contract: kit-abi
kind: c-abi
state: frozen
owner: include/engine
header: include/engine/engine_api.h
implementations:
  - real: src/runtime/scripting/engine_api.cpp
  - null: none
tests:
  - tests/api_abi_compat_test.cpp
  - tests/component_abi_test.cpp
covers:
  - include/engine/engine_api.h
  - include/engine/engine_api_table.h
  - include/engine/contract.h
verified: 2026-09-27
---

# kit-abi — the frozen C ABI kits and games are built against

The engine's public, versioned surface: C types and a table of function groups
(`engine_api_table.h`), plus versioned, layout-folded component contracts
between kits (`contract.h`). Kits built against one engine version load in
another as long as the groups they use exist; a kit whose component contract
disagrees with an already-loaded one is refused at load with both sides named.

## Nothing
There is no null host: a kit only exists inside an engine. "Nothing" is per
call and documented at each call — e.g. `frameAlloc` returns null when unbound,
for size 0, or past the arena, and that is a normal outcome to handle, not an
error to assert on. `engineUiSetBackend(null)` means "no UI": UI calls no-op.

**On a platform with no dynamic loader there is no module path at all.** Game
code written as a module (`ENGINE_GAME_MODULE`) is found only through
`dlopen`/`LoadLibrary` (`engine_build` → `engine_player` + `kits/`). The
alternative that exists today is not a module: a game that links
`engine::runtime` itself registers `IEnginePlugin` classes directly with
`engine.plugins().add(...)`. Statically linked *modules* are WO-025.

## Ownership
Frame-scoped memory from `frameAlloc` is valid until the end of the current
frame and is never freed by the caller. Component contracts: the first module
to declare a contract pins it; later modules must match version and layout.

## Threading
Jobs are the engine's worker pool, not a thread library — a kit must not spawn
its own threads. Work that must not run off-thread (window, GPU, UI) is
deferred to the main thread and runs at the next pump.

## Timing
Deferred main-thread work is fire-and-forget: it runs at the next pump and has
no completion signal. Everything else is synchronous.

## Errors
Logging goes through the engine (`engineLog*`); warnings and errors reach the
game console regardless of level masks. Version or layout skew is refused at
module load — "a build error's moral equivalent, delivered at the last safe
moment" — never discovered at runtime.
