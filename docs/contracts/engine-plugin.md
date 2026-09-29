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
tests:
  - tests/providers_test.cpp
covers:
  - src/runtime/plugin.h
  - src/plugins/stock_plugins.h
verified: 2026-09-27
---

# engine-plugin — physics, scripting, audio and kits as swappable providers

`IEnginePlugin`: attach/detach, simulation start/stop, and per-phase hooks
(`onUpdate` → `onPhysicsStep` → `onPostPhysics`, then render-rate `onFrame`).
Which physics/scripting/audio implementation is constructed is PROJECT DATA
(`project.json` "providers"), chosen in `stock_plugins.h`.

## Nothing
The null providers (`NullPhysicsPlugin`, `NullScriptPlugin`) attach and do
nothing; a project selects them with `"physics": "none"` / `"scripting": "none"`.

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
Not yet written.
