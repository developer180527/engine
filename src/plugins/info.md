# Plugins

## Purpose
Engine subsystem implementations behind the `IEnginePlugin` interface
(`src/runtime/plugin.h`): physics, scripting, audio. Registered with the
runtime's `PluginRegistry`; the host (editor or game) chooses which to add.

## Implementations
- **`JoltPlugin`** (`jolt_plugin.h`) — JoltPhysics. Creates bodies from
  `RigidBody`/`CharacterController` components on sim start, steps at fixed
  60 Hz, writes back transforms, queues collision events and flushes them in
  `onPostPhysics`. Also implements `IPhysicsService` (raycasts for scripts).
- **`LuaScriptPlugin`** (`lua_script_plugin.h`) — sandboxed Lua 5.4
  (no io/os/package/debug). Each `ScriptComponent` entity gets an instance
  table; lifecycle `onStart/onUpdate/onDestroy`; modules cached per path and
  cleared on Stop so Play reloads edited scripts. FFI driven by MetaRegistry
  component schemas.
- **`AudioPlugin`** (`audio_plugin.h` + `audio_impl.cpp`) — miniaudio
  engine; implements `IAudioService` for scripts.
- **`NullPhysicsPlugin` / `NullScriptPlugin`** — stand-ins that draw a
  "not installed" editor panel; useful as minimal plugin examples.

## Rules
- `onAttach(RuntimeContext&)` — capture services, allocate global state.
  The game world does NOT exist yet.
- Game-world pointers are valid only between `onSimulationStart` and
  `onSimulationStop`.
- Per-frame order is guaranteed: `onUpdate` (all plugins) → `onPhysicsStep`
  (all) → `onPostPhysics` (all).
- Editor UI goes through `IEditorPlugin` (`src/editor/editor_plugin.h`) —
  inherit both interfaces. Keep ImGui usage out of simulation code paths so
  a game build never needs ImGui.

## Future Work
- Split plugin headers into backend (runtime-clean) + editor UI parts.
- Service locator instead of concrete plugin types in script bindings.
