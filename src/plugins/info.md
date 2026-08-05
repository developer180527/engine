---
status: as-built
tier: working
verified: 2026-08-05
covers:
  - src/plugins/
tests:
  - tests/providers_test.cpp
  - tests/stress_physics.cpp
---
# Plugins

## Purpose
Engine subsystem implementations behind the `IEnginePlugin` interface
(`src/runtime/plugin.h`): physics, scripting, audio. Registered with the
runtime's `PluginRegistry`; the host (editor or game) chooses which to add.

## Plugin vs Kit (same interface, different deployment)
`IEnginePlugin` is just the lifecycle hook — both built-in subsystems and
external Kits implement it. They differ only in *where they live and how they
load*:
- **Plugin** (this directory) — ships in-tree, compiled *into* `engine_runtime`,
  registered statically at boot.
- **Kit** — its own repo, compiled as a `.so` module, loaded dynamically from
  `project.json` at Play by `KitHost` (`src/runtime/kit_host.h`) over the
  module contract `include/engine/game_module.h`.
Both run through the same `PluginRegistry` broadcasts.

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
  **The device starts on a JOB, not on the main thread.** Profiling
  `engine_host --frames 1` put 585 of 1 722 main-thread samples — ~536 ms, 34% of
  startup — inside `ma_engine_init`, all of it BLOCKED in `ma_device_start` ->
  CoreAudio's `HALB_IOThread::StartAndWaitForState`. The main thread was waiting for
  an audio device before the first frame could be drawn, which nothing on screen
  depends on. `ma_engine_config::noAutoStart` + `jobs::run` moved it off the boot
  path (startup 2.60 s -> 1.16 s); `m_ready` flips only once the device is live, so
  a sound requested in that window no-ops exactly as it would if audio had failed.
  `onDetach` waits on that job before `ma_engine_uninit` — safe only because
  `EngineRuntime::shutdown` detaches plugins BEFORE `jobs::shutdown()`. Do not
  reorder those two.
- **`NullPhysicsPlugin` / `NullScriptPlugin`** — stand-ins that draw a
  "not installed" editor panel; useful as minimal plugin examples.

## Rules
- `onAttach(RuntimeContext&)` — capture services, allocate global state.
  The game world does NOT exist yet.
- Game-world pointers are valid only between `onSimulationStart` and
  `onSimulationStop`.
- Per-frame order is guaranteed: `onUpdate` (all plugins) → `onPhysicsStep`
  (all) → `onPostPhysics` (all).
- Editor UI goes through `IEnginePlugin::onEditorUI()`, drawn with the
  `engineUi*` facade (`include/engine/engine_api.h`) — never ImGui directly.
  The editor registers an ImGui backend; hosts without a UI surface no-op. This
  keeps plugins/kits free of any ImGui/editor dependency (works in `engine_host`
  too). (`IEditorPlugin` is retired.)

## Future Work
- Split plugin headers into backend (runtime-clean) + editor UI parts.
- Service locator instead of concrete plugin types in script bindings.
