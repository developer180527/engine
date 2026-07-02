# Runtime Core

## Purpose
The engine SDK's core: owns the platform (OS window), the ECS world, all
content registries, the renderer, plugins, and the frame loop. A game links
`engine_runtime` and uses this subsystem as its entry point — no editor code
required.

## Architecture
- **`EngineRuntime`** (`runtime.h/.cpp`) — the root object. Owns everything
  below, wires it together in `init()` driven by `EngineConfig` (project root,
  window size, asset database opt-out). Construction order matters: content
  registries are declared before `m_ctx` and `m_renderer` so addresses are
  stable and destruction order is reversed.
- **`IPlatform`** (`platform.h`) — window/event abstraction.
  `GlfwPlatform` is the default (stock OS window); `HeadlessPlatform` returns
  a null native handle, which makes the runtime skip bgfx entirely (servers,
  CLI tools, tests). Custom platforms embed the engine in existing windows.
- **`Renderer`** (`renderer.h/.cpp`) — owns the GPU device lifecycle,
  framebuffers, and the swappable `IRenderPipeline`. Borrows the ECS world and
  registries from `EngineRuntime`. See `src/render/info.md` for the pipeline.
- **`RuntimeContext`** (`runtime_context.h`) — the editor-free service bundle
  handed to plugins, services, and (wrapped) editor panels.
- **`PluginRegistry`** (`plugin_registry.h`) + **`IEnginePlugin`**
  (`plugin.h`) — engine subsystem plugins (physics, scripting, audio) with a
  strict per-frame phase order: `onUpdate` → `onPhysicsStep` →
  `onPostPhysics`. Editor UI is NOT part of this interface — see
  `src/editor/editor_plugin.h`.
- **`AssetService`/`SceneService`** — async asset loading (worker thread
  decode, main thread GPU upload) and binary scene loading.
- **`AsyncLoader`** (`async_loader.h/.cpp`) — legacy import path used by the
  editor for source-format assets (FBX via Assimp etc.).
- **Input** (`input_system.h`, `input_map.h`) — polled GLFW state with
  action/axis bindings; chains scroll/char callbacks so ImGui keeps working.

## Frame Loop
The runtime owns the loop skeleton; apps supply the body:
```
engine.run([&](float dt) { ...; engine.tick(dt, view, proj); ... });
```
`frameBegin(dt)`: poll events → minimize-wait → resize → clamped dt (≤50ms)
→ drain async GPU uploads. `frameEnd()`: `bgfx::frame()`. `tick()`: gameplay
systems → animation → `flecs::world::progress()` → scene render.

## Invariants
- One `EngineRuntime` per process. Not thread-safe; tick on the main thread.
- bgfx runs single-threaded (`renderFrame` called before `init`).
- GPU uploads must happen on the main thread (drain queues, not workers).
- `RuntimeContext` raw pointers are wired in `initSystems` — all non-null
  after a successful `init()` except `assetLib` when `openAssetDatabase=false`.

## Project Lifecycle
A project can open at init (`cfg.projectRoot`, or auto-detect of the
last-opened project) or later via `openProject(root)` — the editor hub boots
with `cfg.autoDetectProject = false` and opens the picked project afterward.
`openProject` loads project.json, opens + scans the asset database, points
AssetService/SceneService at the project, and retitles the window
(`IPlatform::setTitle`). `hasProject()` reports the current state.

## Simulation Lifecycle
`startSimulation(InPlace|Snapshot)` / `stopSimulation()` / `tickSimulation(dt)`.
InPlace simulates the runtime's own world (game: boot = play). Snapshot
serializes the world and simulates a fresh copy (editor Play; Stop restores
the editing world untouched). `simWorld()` returns whichever is active.
The game-facing `tick(dt)` overload runs systems + simulation + a primary-
camera render to the backbuffer (`renderToBackbuffer`, invalid FB target).

## Kits (project plug-ins)
Kits are reusable C++ gameplay systems built ON the engine (FPS controller, IK,
water, ...), each its own repo. A project lists them in `project.json`'s `kits`
array (parsed eagerly into `ProjectContext::kits`); the code loads LAZILY.
`KitHost` (`kit_host.h`) is owned by the runtime and driven by the simulation
lifecycle: `start()` dlopens + attaches manifest kits inside `startSimulation`
(before the SimStart broadcast), `poll()` hot-reloads any changed `.so` in
`tickSimulation`, `stop()` detaches + dlcloses them in `stopSimulation` (after
the SimStop broadcast). Loading defers to Play because kits are *simulation*
plugins — while not playing, no `.so` is held open, so kits rebuild freely.
Manifest `"requires": [names]` orders loading (topological sort in `start()`;
ordering ONLY — kits never link each other's code; unknown deps warn, cycles
fall back to manifest order). Single kits can be unloaded/reloaded mid-play
(`unloadOne`/`loadOne`, surfaced as Plug-in Manager buttons via
`EngineRuntime::kitLoad/kitUnload`); a mid-play load runs its own
`onSimulationStart` since the broadcast already fired.
The dynamic-library load + ABI gauntlet is shared with `engine_host` via
`module_loader.h` (`ModuleLibrary` + `GameModuleAdapter` + `ModuleWatcher`);
the loader is cross-platform (dlfcn on macOS/Linux, the Win32 loader on
Windows behind a small `libOpen`/`libSym`/`libClose` shim). Hosts that dlopen
modules need `ENABLE_EXPORTS` so kits resolve engine symbols (engine_host,
editor). A shipped game instead LINKS the kit library and registers the plugin
directly — no manifest, no dlopen.

## Public API
`include/engine/*.h` umbrella headers are the supported surface; consumers
link the `engine::runtime` alias target. `samples/minimal_game` is the
living proof that the API builds a game with no editor code.

The flat C API (`engine/engine_api.h`, impl `scripting/engine_api.cpp`) is the
*one doorway* kits/modules/Lua call — input, time, entities, transform,
physics, audio, assets, scenes, cursor, plus the **editor-UI facade**
(`engineUi*`). UI calls route to a host-registered `EngineUiBackend` (the editor
provides one over ImGui); with no backend (engine_host / headless) they no-op,
so a kit's `onEditorUI()` is safe everywhere. Full reference: `docs/engine-api.md`.

## Distribution
Two supported modes:
- **Vendored**: `add_subdirectory(engine)` + link `engine::runtime`.
- **Installed**: `cmake --install build --prefix <sdk>` produces a
  relocatable SDK (lib/ archives, include/ headers, bin/engine_cook,
  lib/cmake/EngineRuntime). Consumers `find_package(EngineRuntime)`.
  The config replays the bundled libraries' INTERFACE_COMPILE_DEFINITIONS
  (EngineRuntimeDefinitions.cmake, generated at build time) — Jolt traps
  (SIGTRAP) at runtime if consumers compile with mismatched JPH_* defines,
  so never hand-trim that list.

## Accepted Tradeoffs (reviewed, kept deliberately)
Documented so these don't get re-litigated from scratch — each lists the
trigger that would revisit it:
- **EngineRuntime as composition root** — it owns everything by design;
  the rule is "wiring lives here, logic lives in subsystems". Revisit if
  runtime.cpp grows real logic.
- **RuntimeContext service locator** — raw-pointer service bundle trades
  explicit dependency declarations for simplicity. Misuse is mitigated by
  lifecycle guards (init/attach/tick order is enforced with loud errors).
- **Single-threaded frame orchestration** — Jolt parallelizes internally,
  asset decode is off-thread; the frame itself is sequential. Trigger:
  the planned shared job system (flecs set_threads + engine::jobs).
- **Snapshot play mode via full serialization** — editor-only cost.
  Trigger: noticeable Play-button latency on large scenes.
- **Input singletons + single window** — InputSystem/InputMap are global,
  digital-only axes, linear action lookup (N≈6, hash-keyed). Key/MouseButton
  are engine-owned constants (GLFW-free header; backend static_asserts the
  values). Triggers: gamepad support (analog axes), multi-window editor.
- **ScriptHost as THE gateway** — one deliberately-monolithic stable
  surface shared by Lua/C API/future C#. Keep it minimal; never split it
  per-domain (that relocates coupling without removing it).
- **Per-world query caches** (WorldQueryCache) — queries against the sim
  world are cached and MUST be reset when that world dies; the runtime
  resets its own (and renderer/animator) in stopSimulation.

## Layering
`engine_core` (alias `engine::core`) is the GPU-free layer underneath:
cookers, cook service, and the single-header library implementation TUs
(stb/cgltf). It links only assetlib + assimp — no bgfx, no GLFW, no
Jolt/Lua. `engine_cook` links engine_core alone; `engine_runtime` links
engine_core plus the graphics/platform/plugin stack. Keep new sources on
the right side of this line: if a .cpp references no bgfx/GLFW symbols
and serves data processing, it belongs in engine_core.
