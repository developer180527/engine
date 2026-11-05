---
status: target
covers:
  - src/
  - include/engine/
  - docs/plans/engine-architecture.md
  - docs/plans/engine-api.md
---
# Architecture

The map of the engine: what it is made of, the rules every part follows, and
where each part is described in depth. It is short on purpose; the linked
documents hold the detail.

Each section is marked:
- **Built:** in the tree today.
- **Planned:** agreed or proposed, not built yet. The work order or plan is named.

## 1. What the engine is

A game engine built **as a library first**. A game is a program that links the
engine; the editor and the tools are programs that link the same library.

```
  your game · kits · editor · CLI tools · Python / Swift / JS packages
  ─────────────────────────── public API (C) ───────────────────────────
  CORE (fixed, private)                    PROVIDERS (chosen per project)
  memory · jobs · ECS world · scenes       rendering · physics · audio
  assets and loading · frame loop          scripting · networking · UI
  platform, windows, input · logging       … and new kinds, as services
  ─────────────────────────────────────────────────────────────────────
  libraries the engine stands on: librhi (GPU) · libgui (UI) · flecs · Jolt …
```

## 2. The rules

Every design question is answered by one of these. Each rule is a decision
record (DR-0009, DR-0011, DR-0028 to DR-0035), so a change that breaks one needs
a new record that reverses it first (`docs/process/decisions/`).

| # | Rule | Source |
|---|---|---|
| 1 | **Core is fixed but private; providers are swappable behind contracts.** The ECS is core. The public surface names only the engine's own types, never flecs, bgfx or any other library. | DR-0029, [engine-architecture.md](docs/plans/engine-architecture.md) §1 |
| 2 | **One C API is the only public boundary.** C, so that any language can bind to it and it stays stable across compilers. C++ is a wrapper over it. What we promise, we check. | DR-0028, [engine-api.md](docs/plans/engine-api.md) (draft) |
| 3 | **The host owns the loop and the windows.** The engine does one frame when asked and never pumps events on its own. It sleeps only when asked to pace (`ENG_PACING_ENGINE`); with host pacing it never blocks. | DR-0030, [platform-embedder.md](docs/plans/platform-embedder.md) |
| 4 | **The runtime reads cooked content only, and builds nothing.** The one exception is the GPU driver's last step for compiled shaders, done at load. | DR-0009, WO-018, WO-068 |
| 5 | **Cooked files are in the engine's own formats.** A provider's own form is a cache built from them, never the only copy. | DR-0031, WO-069 |
| 6 | **Development machinery is absent from a shipped game:** not disabled, absent from the binary. Checked, not trusted. | DR-0032, engine-architecture.md §2a, WO-066 |
| 7 | **Capacity grows with content.** An empty game costs almost nothing. | DR-0011, `empty_game_budget_test` |
| 8 | **Every tool is a CLI over the library,** so people and agents can do what the editor does. | DR-0033, engine-architecture.md §7a |
| 9 | **Old data migrates, never breaks.** Unknown components are kept, not dropped. | DR-0034, engine-architecture.md §4 |
| 10 | **Refuse by name, never fall back silently.** A missing provider, kit or asset is an error that names it. | DR-0035, engine-architecture.md §7 |

## 3. Two kinds of user

| | Library user (primary) | Editor user |
|---|---|---|
| Gets | the engine as a library, plus the tools | a prebuilt engine, editor and tools |
| Writes | games, kits, new providers | kits (C or C++) and Lua scripts |
| Contract | the C API (DR-0028) | the same C API (today: the kit ABI) |

**Built:** library users link `engine::runtime` (C++); editor users build kits
against the kit ABI ([kit-abi.md](docs/contracts/kit-abi.md)).
**Planned:** both stand on one C API (rule 2), so the two columns share one
contract.

## 4. The public boundary

**Built:**
- `include/engine/engine_api.h` and `engine_api_table.h`: the kit ABI. It is C,
  with frozen, versioned groups of functions, and a kit built for an older
  version still loads. Why it is shaped this way:
  [extension-model.md](docs/architecture/extension-model.md) §1.
- `include/engine/engine.h`: the C++ SDK. It still includes private headers from
  `src/`, so it is not yet a real boundary.

**Planned** ([engine-api.md](docs/plans/engine-api.md), **----DRAFT------**):
- One C API with the `eng` prefix: engine, windows, views, input, world, assets,
  modules and services. The headers are in `docs/plans/engine-api-draft/`.
- Programs link its symbols. Kits look functions up by name at load, so no table
  layout has to be frozen.
- From 1.0, a stable promise covering source, binary, behaviour and data within a
  major version, checked in CI against a recorded release and against old binaries.
- Generated bindings for Python, Swift, JS (WASM) and Rust. Bulk data crosses as
  arrays, not per-entity calls.

## 5. Core

| Part | Built | Planned |
|---|---|---|
| Memory | `mem::` over TLSF: tagged heaps, a per-frame arena | |
| Jobs | `engine::jobs` over enkiTS | idle cost of the pool (WO-065) |
| World | flecs, used directly inside core | an engine-owned world API; no flecs at the boundary (engine-architecture.md §3) |
| Scenes | a hand-written serializer for a fixed list of components | a schema registry, one generic serializer, unknown components kept (§4) |
| Change stream | the renderer's retained-scene table (WO-019) | one per component type, free without subscribers (§5) |
| Assets | cooked-only loading; a missing cook becomes a cook request in the editor | engine-owned animation and audio formats (WO-069) |
| Frame loop | `EngineRuntime::frameBegin/tick/frameEnd`; fixed-step simulation, replays (`take.h`) | `engStep` as the primitive; the engine paces itself (WO-061) |
| Platform | `IPlatform` (GLFW, SDL3, headless): one game window | first-class windows, monitors and events for games (§8) |
| Input | actions from `input.json`; intents for replayable simulation; raw HID (`modules/hid`) | per-window routing; actions fed from any source |
| Logging, profiling | categorised log ring; profiler channels | compiled out of shipped games (WO-066) |
| Crash reporting | | in every shipped desktop and server build ([crash-reporting.md](docs/plans/future-plans/crash-reporting.md)) |

## 6. Providers and services

A provider is a swappable part with a contract, a null version and a
conformance suite. A **service** is a named, versioned table of functions
(`engine.physics.v1`, `voxel.world.v1`), which is how a provider offers anything,
including kinds the engine has never heard of.

| Kind | Built | Contract | Planned |
|---|---|---|---|
| Rendering | bgfx forward renderer, picked in code | `renderer.md` (bgfx-shaped) | the renderer contract; librhi AAA renderer; WebGPU renderer for the web ([renderer-contract.md](docs/plans/renderer-contract.md)) |
| Physics | Jolt, or none, by name in `project.json` | `engine-plugin.md` | the world API instead of `flecs::world` |
| Audio | miniaudio, or none | `audio-provider.md` (C ABI) | takes the engine audio format (WO-069) |
| Scripting | Lua, or none | `script-services.md` | precompiled Lua in shipped games (WO-068) |
| Networking | `modules/net` (Rust), no contract | | a contract when a second implementation is wanted |
| Game UI | | | libgui, as `engine.ui.v1` (WO-067) |

**Built:** providers are chosen from a hard-coded switch
(`src/plugins/stock_plugins.h`), and an unknown name falls back to the default.
**Planned:** a registry: providers register by name and by service, an unknown
name is refused, and exported games link their providers statically (WO-025).

## 7. One frame

**Built:** `frameBegin` pumps OS events, waits while the window is minimised,
and handles resizes. `tick` runs the fixed simulation steps and the systems.
`frameEnd` extracts what to draw and renders it.

**Planned** (rule 3):
1. The host gathers events (`engPumpEvents`), or pushes them when it owns the window.
2. `engStep` runs any fixed simulation steps that are due (deterministic and
   replayable), then the per-frame systems.
3. Extraction: providers read the world and the change stream, read-only, at
   one point in the frame.
4. Each view renders into its window or offscreen target; the primary window's
   display sets the rhythm; nothing waits on a minimised window.

## 8. Windows, views and input

**Built:** one game window, with title, size and HDR. Extra windows exist only
for the editor (`IToolWindow`, `wsi::`), and games may not use them.

**Planned** ([engine-api.md](docs/plans/engine-api.md), `eng_window.h`, `eng_view.h`):
- **Windows** for games: windowed, borderless or exclusive fullscreen; resize
  limits and aspect; monitor choice and listing; position; window events, with a
  close request the game may decline. A fullscreen-only game sets the mode and
  blocks the OS's own fullscreen toggle.
- **Several windows** where the platform allows. Platforms report what they can
  do, and the web and phones report one window.
- **Views:** one camera on one world, drawn to a rectangle of a window or an
  offscreen target. A window can hold several views; several windows can show
  one world.
- **Embedding:** a window owned by SwiftUI, Qt or a browser canvas becomes an
  ordinary engine window.
- **Input:** raw events carry their window; gameplay uses actions, which any
  source can feed (keyboard, gamepad, a custom cockpit panel).

## 9. Assets, from source to shipped game

```
 source files ──import──▶ cook ──▶ cooked engine formats ──package──▶ dist/
 (FBX, glTF, PNG,          (dev      (meshes, scenes, materials,   (only what
  WAV, Lua …)               only)     shaders, clips, audio)        is used)
                                         │
                                         └──▶ provider caches (ozz runtime form,
                                              GPU texture formats): derived,
                                              rebuildable, never the only copy
```

**Built:**
- Import and cook run in the cook stack (`engine_cooking`), which shipped
  binaries never link. Risky importers run in separate worker processes.
- Meshes, scenes, materials and shaders cook to engine formats.
- `engine_build` packages the meshes and textures that scenes reference, all
  shaders and materials, and animation clips. From `assets/` it takes audio and
  Lua by file extension.

**Planned:**
- Animation without ozz archives inside, and an engine audio format (WO-069).
- Nothing built at runtime (WO-068).
- Audio and scripts packaged by reference (WO-066).
- A derived-data cache and a cook farm
  ([asset-cook-architecture.md](docs/architecture/asset-cook-architecture.md)).

## 10. Development time and shipped games

| Development time only | In a shipped game |
|---|---|
| editor, CLI tools, importers, cooker, shader compilers | the runtime library and the game |
| source assets, the asset database, file watchers | cooked assets for that platform and its providers |
| hot reload; loading kits and providers by name | the chosen kits and providers, linked in |
| validation layers, debug consoles, the profiler UI | crash reporting |

**Built:** `engine_build` compiles a separate player with no cook stack, and
`player_has_no_cook_stack` checks its symbols.
**Not yet:** hot reload, the kit loader, engine log output and the profiler
channels are switched off at runtime but still in the binary (WO-066). The kit
loader goes when kits link statically (WO-025).

## 11. Rendering

**Built:** a forward renderer on bgfx, with HDR output and a colour pipeline.
**Planned:**
- [librhi](https://github.com/developer180527/librhi), a separate GPU library
  (C ABI; Metal 4, Vulkan 1.3, D3D12; DR-0012), under an AAA renderer.
- Shaders are written in Slang (DR-0013).
- Every renderer stands behind one contract that names no GPU API: it reads the
  world, owns GPU residency, declares the cook profiles it needs, and draws into
  surfaces the engine provides. The web gets its own WebGPU renderer
  ([renderer-contract.md](docs/plans/renderer-contract.md)).

## 12. UI

**Built:** the editor uses ImGui. libgui (a separate UI core in Rust with a C ABI)
is an experiment in `experiments/libgui_sandbox`, off by default.
**Planned:** game UI on libgui: HUDs, menus, and panels drawn onto 3D surfaces,
with a rule that no game ever reaches editor code (WO-067). Moving the editor to
libgui is a separate decision.

## 13. Code map

| Directory | Holds |
|---|---|
| `src/core/` | memory, jobs, logging, math; depends on nothing else in the engine |
| `src/runtime/` | frame loop, platform and windows, input, services, module loading, replays |
| `src/components/`, `src/systems/` | shared components and the systems that run them |
| `src/scene/` | scene serialization |
| `src/assets/` | importers and cookers (development time only) |
| `src/animation/`, `src/audio/` | animation runtime (over ozz), the miniaudio provider |
| `src/render/` | the bgfx renderer |
| `src/plugins/` | providers: physics, scripting, audio |
| `src/editor/` | the editor; nothing outside it may include it |
| `src/tools/` | CLI tools: player, cook, build, project, module probe |
| `include/engine/` | the installed SDK (kit ABI) |
| `modules/` | separate libraries: `assetlib`, `hid`, `net` (Rust) |
| `experiments/` | off-by-default trials (libgui front end) |
| `docs/plans/engine-api-draft/` | the draft public C API (not built, not installed) |

## 14. Where to read next

| For | Read |
|---|---|
| where things stand right now | [ENGINE_STATUS.md](ENGINE_STATUS.md) (generated), `python3 scripts/work_orders.py brief` |
| what to do next | [docs/work/BOARD.md](docs/work/BOARD.md) |
| why something is the way it is | [docs/process/decisions/](docs/process/decisions/README.md) |
| the target shape in detail | [engine-architecture.md](docs/plans/engine-architecture.md), [engine-api.md](docs/plans/engine-api.md), [renderer-contract.md](docs/plans/renderer-contract.md) |
| how code attaches to the engine | [extension-model.md](docs/architecture/extension-model.md), [provider-abi.md](docs/architecture/provider-abi.md) |
| contracts between parts | [docs/contracts/](docs/contracts/README.md) |
| all documentation | [docs/README.md](docs/README.md) |

**Open questions for the owner** are listed at the end of
`engine-architecture.md` (§9), `engine-api.md` (§11) and `renderer-contract.md` (§6).
