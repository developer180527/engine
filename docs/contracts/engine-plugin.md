---
status: as-built
contract: engine-plugin
kind: interface
state: provisional
owner: src/runtime
header: src/runtime/plugin.h
implementations:
  - real: src/plugins/jolt_plugin.h#JoltPlugin
  - real: src/plugins/lua_script_plugin.h
  - real: src/plugins/audio_plugin.h#AudioPlugin
  - null: src/plugins/null_physics_plugin.h
  - null: src/plugins/null_script_plugin.h
  - null: src/plugins/null_audio_plugin.h
tests:
  - tests/providers_test.cpp
covers:
  - src/runtime/plugin.h
  - src/plugins/stock_plugins.h
  - src/plugins/stock_providers.cpp
  - src/runtime/provider_registry.h
verified: 2026-11-26
---

# engine-plugin — physics, scripting, audio and kits as swappable providers

`IEnginePlugin`: attach/detach, simulation start/stop, and per-phase hooks
(`onUpdate` → `onPhysicsStep` → `onPostPhysics`, then render-rate `onFrame`).
Which physics, scripting, audio and renderer implementation is constructed is
PROJECT DATA (`project.json` "providers"), looked up by name in the provider
registry (`src/runtime/provider_registry.h`). The build's one list of providers
is `src/plugins/stock_providers.cpp` (WO-076). `engine_player --list-providers`
prints it.

## Nothing
Every kind has a `none` provider (`NullPhysicsPlugin`, `NullScriptPlugin`,
`NullAudioPlugin`, `NullRenderer`). It attaches and does nothing, and is chosen
by name, e.g. `"physics": "none"`. It is never used as a fallback.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
The game world is only valid between `onSimulationStart` and
`onSimulationStop`. Per frame while simulating, in this order: `onUpdate`
(fixed dt; gameplay reads input, sets intent), `onPhysicsStep`,
`onPostPhysics`; then `onFrame` once per rendered frame for presentation-only
work — gameplay state never changes there.

## Errors
A provider name this build does not have, a kind the engine does not know, or a
value that is not a name is refused at startup, by name, with the names this
build has (DR-0035). The renderer is checked in `EngineRuntime::init()`, before
anything is created. Physics, scripting and audio are checked in
`addProviders()`, which adds nothing if any choice is refused. The reason is in
`EngineRuntime::bootError()`, and the C API returns it as `ENG_ERROR_NOT_FOUND`.
A host may `inject()` its own provider object for a kind; the project's name for
that kind is then not looked up.
