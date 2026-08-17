---
status: as-built
tier: hardened
verified: 2026-08-17
parses-external-input: true
covers:
  - src/runtime/
tests:
  - tests/sim_world_test.cpp
  - tests/asset_ready_test.cpp
  - tests/async_loader_test.cpp
  - tests/script_host_test.cpp
  - tests/kit_lifecycle_test.cpp
  - tests/residency_test.cpp
  - tests/api_primitives_test.cpp     # the jobs/memory/drawSubmit contract
  - tests/fuzz_scene_loader_test.cpp   # the cooked scene SceneService consumes
  - tests/fuzz_scene_service_test.cpp  # SceneService itself, over hostile scenes
  - tests/mesh_dedup_test.cpp
  - tests/material_name_test.cpp
  - tests/input_test.cpp
  - tests/input_config_test.cpp
  - tests/nav_test.cpp
  - tests/prev_snapshot_test.cpp
  - tests/sim_purity_check.cpp
  - tests/soak_engine.cpp
# NOT `hardened`, despite being the most-tested subsystem here (11 tests incl.
# a days-long soak). Endurance evidence is overwhelming; HOSTILE-INPUT evidence
# is not. The runtime parses input.json itself (hardened by inspection in audit
# H.4, covered by input_config_test — but never fuzzed), and SceneService reads
# cooked scene binaries whose byte-level parse lives in assetlib. Volume of
# tests is not the same as adversarial coverage.
# Blocker to `hardened`: a fuzz target for the scene deserializer (already the
# next planned target in docs/plans/automated-testing-soak-fuzz-plan.md) and/or
# input.json. Raise the tier when one exists — not before.
---
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
  `PlatformConfig::hideTitleBar` suppresses the OS title bar's DRAWING while
  keeping a fully decorated window — resize, snap, minimise, maximise and the
  window buttons all keep working. It is deliberately NOT an undecorated
  window; `platform/title_bar.h` carries the reasoning and the per-platform
  state (macOS implemented, Windows written-but-unverified, Linux falls back to
  a real bar). Only the editor sets it: a shipped game window keeps its title
  bar. Title-bar suppression reaches past GLFW/SDL to the native window, so
  CMake selects the implementation by OS rather than by window backend — which
  is why adding it did not fork the two window backends.
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
  decode, main thread GPU upload) and binary scene loading. Meshes and textures
  are addressed by cooked PATH; **materials by authored NAME** (`"rust"`, not
  `<uuid>.cooked`) resolved through an index over `<cache>/materials`, because a
  shipped dist has no registry to turn a path into a uuid. Two invariants there:
  one Material per name (a spawner calls it per entity, so repeat calls return
  the same handle), and `unloadMaterial` must evict that name — `MaterialHandle`
  is a bare slot index over a free list with no generation counter, so a stale
  cache entry does not go invalid, it starts naming whatever material next took
  the slot. Pinned by `tests/material_name_test.cpp`. `textureCache()` exposes the
  cooked-texture dedup cache read-only, for the VRAM census — which existed since
  Phase 2 and could never be called, because the cache was private. The census
  only sees the COOKED path: a scene referencing source `.fbx`/`.gltf` loads via
  `AsyncLoader` + the importers, which create textures directly and bypass the
  cache (see `docs/plans/renderer-audit-and-plan.md`).
  MESHES are deduped by cooked path too, as of 2026-08-05, through the same map the
  async path uses — and the fix is worth knowing because of how it failed. That map
  lived inside `AsyncState`, which `ensureWorker()` creates LAZILY, so a scene that
  loaded every mesh synchronously (the cooked fast path) had no cache at all: each
  entity created its own vertex+index buffers, and bgfx's pool is
  `BGFX_CONFIG_MAX_INDEX_BUFFERS` = 4096. A 50 000-entity scene drawing 176 distinct
  cooked meshes loaded 4 089 and then failed 45 911 times, rendering 8% of itself. The
  map now lives on `AssetService` beside `m_texCache`, which is the level it should
  always have been at — textures avoided the bug only because their cache already sat
  there. Found by `scripts/gen_fuzz_scene.py`; every prior stress scene used
  `engine://primitive/cube`, one shared mesh, so nothing ever allocated a second
  buffer pair. Pinned by `tests/mesh_dedup_test.cpp`, which loads ONE cooked path
  5 000 times — past bgfx's 4 096 pool — so it reproduces the original failure instead
  of merely checking that a handle is reused: with the dedup removed it reports 907
  invalid handles, 4 096 buffers and 4 093 registry meshes.
  The residency entry also carries the mesh's **LOD chain**. It has to: a cache
  HIT that returned without handing back the levels gave exactly the first entity
  per mesh a chain and every subsequent one none, so 20 000 objects sharing 176
  meshes produced 176 LOD'd entities — indistinguishable, at scene scale, from LOD
  not working. Each level is a real `Mesh` with its own buffers, since the entire
  point is that it carries fewer triangles, and levels inherit level 0's bounds so
  the cull sphere that chose the level is not altered by the choice.
  `tests/fuzz_scene_service_test.cpp` drives `SceneService::loadScene` itself
  over hostile cooked scenes — real `AssetService`, real flecs world, real
  cooked meshes, bgfx on the **Noop** backend so every handle path runs without
  a GPU. The parse fuzzer (`fuzz_scene_loader_test`) cannot see any of what this
  asserts, because these are the CONSUMER's own failures:

    - a REJECTED load must leave nothing behind. Spawning half a scene and then
      returning 0 is a leak no parse test can detect.
    - load → unload must be exactly balanced against the world's entity count.
      This is the property that makes level streaming possible at all.
    - parent links must terminate. `parentId` is attacker-controlled, so
      self-parent, duplicate ids and 40-deep chains all arrive here.
    - handle hygiene: an unissued or already-unloaded handle is `false`, never
      a crash, and `sceneEntityCount` must agree with what the world gained —
      unload destroys from that list, so a disagreement is a leak or a
      double-destroy waiting to happen.

  6 000 explore cases pass clean; SceneService held up. Since a passing fuzzer
  proves nothing on its own, the properties are mutation-verified: removing the
  `destruct()` in `unloadScene` reports the leak, and swapping `safeReparent`
  for a raw `child_of` aborts inside flecs' childof-depth recursion. The second
  is worth stating precisely — the cycle is caught by flecs dying, not by the
  ancestor-walk assertion, which only covers a cycle flecs would tolerate.
- **The engine API's PRIMITIVE tier** (`engine_api.cpp`, bound in
  `runtime_boot.cpp`). The table long exposed the engine's SUBSYSTEMS —
  physics, animation, nav, audio — which are finished opinions: a developer who
  wants their own animation system cannot build one out of them, only adopt
  ours or fork. `jobs`, `memory` and `drawSubmit` expose what those subsystems
  are built ON, so the engine's animator becomes the DEFAULT rather than the
  only option.
    - `jobs` — the engine's own pool. A kit spawning private threads competes
      with the pool already saturating the machine. Blocking `parallelFor`
      only: a job handle crossing a module boundary means the module owns a
      lifetime the host allocated, and a kit unloaded mid-job takes the process
      with it. Async can be appended in v2, which is what per-group versioning
      is for. `onMain` defers to the main thread, and its queue is DRAINED
      (`jobs::drainMain`) immediately before kits are dlclosed and before plugins
      detach — a queued callback is a function pointer into a dylib, so one left
      pending when that library unmaps is a jump into freed code the next time the
      frame loop pumps.
    - `memory` — tagged heaps plus the frame arena, so kit allocations are
      visible to the budget telemetry instead of hiding in malloc. `frameAlloc`
      REFUSES anything the arena cannot serve FROM WHAT REMAINS, not merely
      anything larger than the arena: `FrameArena` spills to the heap on overflow,
      which suits engine code that overshoots slightly and is wrong for a module
      that can ask for anything. Checking total capacity let a request that fits
      an empty arena through against a nearly full one, which is the spill the
      guard exists to prevent.
    - `log` — a module's own CATEGORY in the engine's ring, as opposed to
      `core.logInfo`'s one-liner (which lands under "Script"). A subsystem gets
      its own row in the Internal Console's table, its own per-level filters and
      its own Solo button. Three properties are load-bearing:
        * THE NAME IS COPIED. A literal in a module's dylib would dangle the
          instant that module unloads, and both the registry and every buffered
          record hold pointers to it — the same class of hazard as a deferred
          `onMain` callback outliving its library. `elog::categoryCopied`.
        * `enabled()` EXISTS SO FORMATTING CAN BE SKIPPED, and `write()`
          re-checks it, so a module that ignores the advice still cannot defeat
          the console's filters.
        * MESSAGES ARE PRE-FORMATTED and passed with `"%s"`. Nothing variadic
          crosses the boundary in either direction: a stray `%n` in a module's
          message would otherwise read the HOST's stack.
    - `drawSubmit` — geometry with no entity and no component, reset once per
      frame by `Renderer::endFrame()`. The primitive for a renderer-facing system
      the engine never modelled. Bound to the renderer at boot and UNBOUND at
      shutdown, or a kit ticking late would submit into a destroyed one.
      THREAD-SAFE, because the tier also ships a job pool and the intended use is
      a kit's particle system; capped at 1 024 submissions a frame, because the
      list is filled by code the engine does not own.
      The reset is deliberately NOT in `Renderer::frame()`: that flips bgfx, so the
      runtime calls it only with a window, while the binding is unconditional — and
      a headless server therefore accumulated every submission for the life of the
      process, ~480 KB/s at 100 a tick, drawn by nobody.
  All three are published unconditionally, including headless: `drawSubmit`
  accepts and discards without a renderer rather than going absent, so a kit that
  draws runs unchanged on a dedicated server instead of branching on capability.
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
Each fixed sim step opens with `EventSweeper::sweep` (`event_sweeper.h`) —
ages every `events::declare`'d component so a message is observable for a
full tick regardless of plugin order — then the interpolation snapshot,
`beginTick`, and the update/physics/post broadcasts. `stopSimulation` calls
`m_eventSweeper.reset()` alongside the other cached-query releases before the
sim world is destroyed (query-outlives-world crash class).

Services (`scripting/script_services.h`: IPhysicsService, IAnimService) take
an explicit `(flecs::world&, flecs::entity_t)` rather than a world-bundling
`flecs::entity`. ScriptHost splits the pair at the boundary from its bound
`m_world`; its public API, the C API, and Lua keep `flecs::entity`.

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
Windows behind a small `libOpen`/`libSym`/`libClose` shim — `libClose` is
deliberately never called; see the graveyard note below). Hosts that dlopen
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
so a kit's `onEditorUI()` is safe everywhere. Full reference: `docs/guides/engine-api.md`.

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
  asset decode is off-thread; the frame itself is mostly sequential. PARTIALLY
  RETIRED 2026-08-04: renderer extraction now runs on `engine::jobs`
  (`jobs::parallelFor` over archetype chunks — see `src/render/renderer/extract.cpp`),
  which was this tradeoff's stated trigger. The rest of the frame is still
  sequential, and the next candidate turned out not to need threading at all:
  `Sim.prevSnapshot` went from 12.7 ms to 0.63 ms per step at 50 000 objects by
  doing per-archetype work instead of per-entity structural ops (issues.md H.0).
  The lesson both share: reach for the query shape before reaching for cores.
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

## Kit images: the graveyard, and what a shipped game pays for it

`ModuleLibrary::unload()` releases contracts and calls the module's own destroy,
then **parks the image in a process-lifetime graveyard — it is never unmapped**,
on either platform. `libClose` exists as the symmetric half of `libOpen` and has
no callers. The reason is in `tests/kit_lifecycle_test.cpp`: kits register flecs
component hooks (ctor/dtor template instantiations compiled INTO the kit), a live
world keeps those pointers, and dlclosing made "unload a kit mid-play, resume,
shoot" jump into unmapped memory. This is the Unreal hot-reload model, and vCAD's
plugin contract reaches the same conclusion from the same kind of crash
(`docs/architecture/kit-abi-lessons.md`).

**What that costs, stated correctly.** A previous version of the comment claimed
shipped games statically link kits and never take this path. They do:
`engine_build` assembles `dist/kits/` from real kit binaries and rewrites
project.json to point at them. The honest accounting is:

| | editor | shipped player |
|---|---|---|
| mapped images retained | one per reload — unbounded by design, the price of iteration | one per kit, bounded; `startSimulation`/`stopSimulation` run once each |
| startup copy of every kit binary | needed — `dlopen` caches by inode, so a rebuild must overwrite the file while the old image is mapped | **not needed**, and now skipped |
| temp file left per kit on exit | dev machine | **none**, now that the copy is skipped |

`KitHost::setReloadPolicy(Reload::Never)` is what removes the last two, and
`engine_player` sets it. The default is `Allowed`, deliberately: a host that
forgets to opt in loses a little launch time, where the opposite default would
silently break hot-reload in the editor.

The one case that CAN grow during play is a game calling `kitLoad`/`kitUnload`
itself (the editor's Plug-in Manager does). Each cycle is one temp copy plus one
permanently mapped image, so a game exposing kit toggling to players should not
do it per level transition.
