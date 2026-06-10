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

## Simulation Lifecycle
`startSimulation(InPlace|Snapshot)` / `stopSimulation()` / `tickSimulation(dt)`.
InPlace simulates the runtime's own world (game: boot = play). Snapshot
serializes the world and simulates a fresh copy (editor Play; Stop restores
the editing world untouched). `simWorld()` returns whichever is active.
The game-facing `tick(dt)` overload runs systems + simulation + a primary-
camera render to the backbuffer (`renderToBackbuffer`, invalid FB target).

## Public API
`include/engine/*.h` umbrella headers are the supported surface; consumers
link the `engine::runtime` alias target. `samples/minimal_game` is the
living proof that the API builds a game with no editor code.

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

## Future Work
- engine_core split (GPU-free layer) to slim headless/CLI consumers.
