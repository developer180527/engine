---
status: unreviewed
---
# engine

A game engine built as a **library first**: a fixed core that makes it a *game*
engine, swappable providers for everything a game may want to choose, and an
editor and tools on top. It runs on macOS, Windows and Linux (x64 and arm64).

<img width="1624" height="1061" alt="The editor" src="https://github.com/user-attachments/assets/f46da2f7-77e4-4698-835b-e98227f8c6aa" />

## Architecture

```
  Your game  ── C++ (library users) ── or ── C++ kits + Lua (editor users) ──┐
                                                                           │
  ┌─────────────────────────── public surface ─────────────────────────────┤
  │  engine::runtime (C++)          kit ABI: include/engine/ (C, versioned) │
  └──────────────────────────────────┬──────────────────────────────────────┘
                                     │
  CORE (fixed, private)              │       PROVIDERS (chosen per project)
  ─────────────────────              │       ──────────────────────────────
  memory (mem::)                     │       rendering   bgfx today; librhi next
  jobs and threading (engine::jobs)  ├──────▶physics     jolt | none
  ECS and world (flecs)              │       scripting   lua | none
  scenes and serialization           │       audio       miniaudio | none
  asset database, loading, cooking   │       networking  modules/net (no contract yet)
  frame loop and time                │
  platform and input plumbing        │       each: a contract, a null version,
  logging, profiling                 │       a conformance suite
```

**The rule:** core is fixed but private; providers are swappable behind
contracts; the public surface uses only the engine's own types. Inside core,
code may call its own libraries directly. What games, kits and providers see
names no third-party library.

**Where that is true today, and where it is not yet:**
- Physics, scripting and audio are chosen by name in `project.json`
  (`"providers": {"physics": "jolt", ...}`, `src/plugins/stock_plugins.h`).
- Jobs and memory hide their libraries (`engine::jobs` over enkiTS, `mem::`
  over TLSF).
- **Not yet:** the renderer is picked in code and its interface is shaped like
  bgfx ([`docs/plans/renderer-contract.md`](docs/plans/renderer-contract.md));
  the ECS leaks flecs into the kit ABI; the scene format is a hand-written list
  that does not keep components it does not know. The plan that fixes all three:
  [`docs/plans/engine-architecture.md`](docs/plans/engine-architecture.md).

**The world is ECS data.** Scenes are entities with components; the runtime
reads **cooked content only** (DR-0009), and a missing cook is a cook request
answered by the editor or the cooker, never a source parse in a shipped game.
Capacity grows with content: an empty game costs almost nothing (DR-0011,
`tests/empty_game_budget_test.cpp`).

**Rendering is moving to its own library.** The renderer runs on bgfx today.
Its replacement stands on [librhi](https://github.com/developer180527/librhi), a separate, reusable GPU library
(C ABI; Metal 4, D3D12, Vulkan 1.3; DR-0012). Shaders are written in Slang
(DR-0013). See [`docs/rhi/`](docs/rhi/) and `renderer-contract.md`.

## Two kinds of user

| | Library user (primary) | Editor user |
|---|---|---|
| Gets | the engine as source, linked as a library (`engine::runtime`) | a prebuilt engine, editor and tools |
| Receives as binaries | only the tools: cooker, packager, profiler (and the editor if wanted) | everything |
| Writes | games, kits, and new providers | C++ kits and Lua scripts |
| Contract | the source API | the **kit ABI** ([`docs/contracts/kit-abi.md`](docs/contracts/kit-abi.md)): C, versioned; a kit built for another version is refused by name |

A shipped game contains the runtime, its providers, its kits and its cooked
assets. Never the cooker or the editor (`player_has_no_cook_stack` checks it).

## What is built

| Target | What it is |
|---|---|
| `engine::core`, `engine::runtime` | the engine as a library; `engine_runtime_server` is the headless build with no graphics |
| `engine::headers` | the SDK headers kits compile against (`include/engine/`) |
| `engine::cooking` | the cook stack (importers, cookers, encoders); linked by tools only |
| `editor` | the editor |
| `engine_player` | runs a cooked project; what a shipped game is |
| `engine_host` | runs a project with its kits hot-reloaded |
| `engine_cook`, `engine_cook_worker` | the offline cooker and its worker processes |
| `engine_build` | packages a project into a distributable build |
| `engine_project` | creates a project from a template |

## Build

Needs CMake, Ninja and a C++20 compiler; Rust (cargo) for the networking module,
or `-DENGINE_WITH_RUST=OFF`.

```bash
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -L unit
```

Options: `ENGINE_BUILD_EDITOR`, `ENGINE_BUILD_TOOLS`, `ENGINE_BUILD_SAMPLES`
(all on), `ENGINE_WITH_SOURCE_IMPORTERS` (off for a shipping build with no cook
stack). Create and open a project:

```bash
build/engine_project create ~/MyGame && build/editor ~/MyGame
```

## Layout

| Directory | What lives here |
|---|---|
| `src/core/` | memory, jobs, logging, math: depends on nothing else in the engine |
| `src/runtime/` | the frame loop, platform, input, services, module loading |
| `src/components/`, `src/systems/` | the shared components and the systems that run them |
| `src/scene/` | scene serialization |
| `src/assets/` | the asset database, importers and cookers |
| `src/render/` | the bgfx renderer, behind `render/gpu.h` |
| `src/plugins/` | the providers: physics, scripting, audio |
| `src/editor/` | the editor |
| `src/tools/` | the command-line tools |
| `include/engine/` | the installed SDK (kit ABI) |
| `modules/` | separate libraries: `assetlib` (asset registry and cook pipeline), `hid`, `net` (Rust) |
| `samples/` | `minimal_game` (the engine as a library), `hot_reload_game` (kits) |
| `tests/` | ctest lanes (labels): unit, docs, audit, asset, determinism, perf, stress, soak, fuzz-regress, fuzz-explore |
| `docs/` | everything else, starting at [`docs/README.md`](docs/README.md) |

## Where to read next

- **Where things stand:** [`ENGINE_STATUS.md`](ENGINE_STATUS.md) (generated
  from the tree) and `python3 scripts/work_orders.py brief`.
- **What to do next:** [`docs/work/BOARD.md`](docs/work/BOARD.md).
- **Why something is the way it is:**
  [`docs/process/decisions/`](docs/process/decisions/README.md).
- **Contracts between parts:** [`docs/contracts/`](docs/contracts/README.md).
