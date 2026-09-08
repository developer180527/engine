---
status: as-built
tier: working
verified: 2026-09-08
covers:
  - src/plugins/
tests:
  - tests/providers_test.cpp
  - tests/stress_physics.cpp
  - tests/determinism_gate_test.cpp   # the physics tier pins the contact sort
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

  **Contacts are SORTED before they reach scripts.** `OnContactAdded`/`Removed`
  run on Jolt's worker threads and push under a mutex, so arrival order is a
  race — and that order became the order of `CollisionEvents::entered`, which
  scripts iterate. Sorted in `dispatchCollisionEvents` on a normalised
  `(min, max, enter, a)` key, at the source and deliberately not in the
  determinism hasher: a defensively-sorting digest would have hidden this while
  gameplay still observed it (BUG-0054). `determinism_gate_test`'s physics tier
  pins it — removing the sort diverges at tick 0 on both comparisons.

  **`unordered_map` is not the non-determinism people assume it is** (measured
  2026-09-08, correcting the claim `3dc1f47` made when it changed these). libc++'s
  `std::hash<uint64_t>` is unseeded identity, so the same keys inserted in the
  same order give the same iteration order, run to run and process to process.
  `m_entityToBody` and `m_characters` are `std::map` anyway — an ordered map's
  sequence is a function of the live key set rather than of insertion history,
  which is what Jolt's `Architecture.md` asks for around BodyID recycling — but
  that is prior art plus robustness, **not** something the gate measures.
  Reverting either leaves it green. The lookup-only maps (`m_bodyToEntity`,
  `m_charState`) stay hashed.

- **`JoltJobsAdapter`** (`jolt_jobs_adapter.h`) — Jolt's `JobSystem` on the
  engine worker pool, so physics does not oversubscribe cores against
  animation and loading. Three things about it are load-bearing:
  - **`jobs::run` is not always asynchronous.** enkiTS runs a task inline on
    the submitting thread when that thread's pipe is full, and Jolt spawns
    collision jobs from inside collision jobs, so under contention a Jolt job
    started on a thread already running one — tripping `JPH::BodyAccess`'s
    thread-local `JPH_ASSERT(velocity == EAccess::ReadWrite)`, or silently
    breaking that guarantee with asserts off (BUG-0055). `QueueJob` now defers
    while nested; `Barrier::Wait` executes anything still deferred, and
    `Job::Execute` is CAS-guarded so running it twice is safe.
  - **The nesting probe wraps the job FUNCTION, not our lambda**, because
    `Barrier::Wait` executes jobs directly on the waiting thread and we do not
    control that call site. It costs one `std::function` construction per job:
    measured 68 ns, ~57 jobs/tick, ~3.9 µs/tick — about 0.24% of the physics
    step in a Debug build. Kept at that price; the free alternative covers only
    the jobs our pool runs, which is not where the bug was.
  - **Destruction is drain-or-leak, decided by the owner.** `QueueJob` is
    fire-and-forget, so `onSimulationStop` calls `drain()` *before* tearing
    down the `PhysicsSystem`, and on a 30 s stall it leaks the adapter rather
    than freeing a job list under a running job. Aborting there would kill the
    editor and the user's unsaved scene; the destructor still aborts, but only
    as the unreachable end of the road when an owner skipped `drain()`.
    **The leak does not make a stalled job safe** — it keeps the `Job` and its
    free list alive, but that job also reaches `m_physics` and a
    `PhysicsUpdateContext` that died with `PhysicsSystem::Update`'s stack frame.
    A 30 s stall is already undefined behaviour; leaking removes one dangling
    pointer of several, and is chosen for being survivable long enough to save,
    not for being correct.
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
