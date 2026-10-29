---
status: plan
covers:
  - include/engine/
  - src/plugins/
  - src/scene/
---
# Engine architecture: a fixed core, swappable providers, two kinds of user

> **Status: plan, 2026-10-29. Nothing here is built.** Agreed with the owner on
> 2026-10-29: the ECS is core (not swappable), and editor users write C++ kits
> as well as Lua. It sits above [`renderer-contract.md`](renderer-contract.md),
> which is this plan applied to one provider kind.

## 1. The principle

**Core is fixed but private. Providers are swappable behind contracts. The
public surface uses only the engine's own types.**

- **Core** is what makes this a *game* engine and what no game replaces:
  memory, jobs and threading, the ECS and world, scenes and serialization, the
  asset database with loading and cooking, the frame loop and time, platform
  and input plumbing, logging and profiling hooks.
- **Providers** are the parts a game may choose: rendering, physics, audio,
  networking, scripting. Each kind has a contract (`docs/contracts/`), a null
  implementation, and a conformance test suite every implementation passes.
- **Inside core**, code may call its own libraries directly (flecs, enkiTS,
  TLSF). **At the boundary** that games, kits and providers see, it may not.
- **The kinds are open, not a list** (§6a): rendering, physics, audio,
  networking and scripting are the kinds that exist today, not the only kinds
  there can be. A provider developer may define a new kind of work and data.
- **Two things are core services, not providers:** crash reporting, which ships
  in every runtime (`future-plans/crash-reporting.md`), and the tools, each a
  CLI over the library (§7a).

## 2. Two kinds of user

| | Library user (primary) | Editor user |
|---|---|---|
| Gets | the engine as source, linked as a library (`engine::runtime`) | a prebuilt engine, editor and tools |
| Binaries they receive | tools only: the cooker, profiler, packager, and the editor if wanted | everything |
| Writes | anything: games, kits, **new providers** | Lua scripts and **C++ kits** |
| Their code is compiled | with the engine, from source | against the prebuilt engine's SDK, then bundled with the runtime and cooked assets |
| Can swap providers | to any provider, including their own | to any provider **shipped prebuilt** with that engine version |
| Their contract is | the source API (C++, may change between versions) | **the kit ABI** (`docs/contracts/kit-abi.md`): C, small, versioned, refused by name on mismatch |

Two consequences, both deliberate:
- **The kit ABI is the editor user's whole contract**, so it stays C, small
  and checked (`componentLayoutHash`, `kAbiRevision`, `kit_abi_headers`,
  WO-051). Writing a *provider* is library-user work, and needs no binary ABI
  (DR-0006 stands).
- **A shipped game contains the runtime, the chosen providers, the kits and the
  cooked assets.** Never the cooker, never the editor (WO-017,
  `player_has_no_cook_stack`).

## 2a. Development time and runtime are different builds

**No development-time mechanism exists in a shipped game: not disabled, not
hidden, absent from the binary.** A flag can be flipped; absent code cannot run.

| Development time only | Runtime (allowed in a shipped game) |
|---|---|
| the editor, the CLI tools, importers, the cooker, the shader compiler | the runtime library |
| source assets, the asset database, file watchers | cooked assets, for that platform and its chosen providers only |
| hot reload; loading providers and kits by name at runtime | the chosen providers and kits, linked in (§2b) |
| validation layers (engine and librhi), debug consoles, the Lua REPL | logging without engine chatter (`quietForShipping`) |
| the profiler UI, debug-draw tooling | crash reporting (§1) |

**Checked, not trusted:** `player_has_no_cook_stack` (WO-017) already reads a
shipped binary's symbols for the cook stack. It grows into a list covering the
whole left column, and a check of the exported folder fails on any source
asset, `.db` file, shader source, or provider the project did not choose.

## 2b. How providers are swapped, per user and per build

- **A registry, not a switch.** Providers register by name; `project.json`
  chooses among what is registered. Today's `stock_plugins.h` is a hard-coded
  switch only the engine's own hosts can extend.
- **Direct injection for library users:** a host may hand the engine a provider
  object itself, without a name, as `init()` already takes a platform object.
- **At development time** (the editor, a library user's dev build) providers
  may be loaded as shared libraries by name, so they swap and reload without a
  rebuild. Editor users swap among the providers shipped prebuilt with their
  engine version.
- **In an exported game** the chosen providers are **linked statically** (iOS,
  consoles and the web cannot load libraries at runtime anyway), so the swap is
  made by `engine_build`, per platform. Prerequisite: static registration
  (WO-025).
- **The same provider code in both:** only the link mode differs.

## 3. The world, behind engine types

**Today (measured 2026-10-29):** 49 source files include flecs directly
(editor 22, runtime 13, components 6, scene 3, render 2, systems 1, plugins 1),
and three installed SDK headers expose it: `game_module.h` includes it and
hands kits an `ecs_world_t*`, `components.h` says to attach components "via
`engine.ctx().ecs`", and `engine_api.h` defines `EngineEntity` as a flecs id.
(`contract.h` relies on flecs matching components by name across kits, but
does not include it.) Every kit and provider is tied to
flecs's version and layout.

**Decision (owner, 2026-10-29): the ECS is core and not swappable.** So flecs
is not wrapped everywhere; that would cost query speed and ergonomics for a
swap nobody wants. What changes is the boundary:

1. **An engine-owned world API** for games, kits and providers: entity handles,
   component access by registered type, queries, and the change stream (§5).
   In the kit ABI it is C; for library users a thin C++ layer over the same
   calls.
2. **Providers get the world through their contract**, never `flecs::world`.
   A renderer gets a read-only view and the change stream
   (`renderer-contract.md` §3.2); physics gets its bodies the same way.
3. **Engine internals keep calling flecs directly**, the editor included (a
   first-party tool built with the engine).
4. **An audit rule**: no flecs include in `include/engine/` or `src/plugins/`,
   with today's findings as a baseline that can only shrink.

The same rule is already met by jobs (`engine::jobs` over enkiTS) and memory
(`mem::` over TLSF). The ECS is the one core library that leaks today.

## 4. A scene no provider owns

**Today:** the serializer is a fixed, hand-written list (`entity_serializer.h`;
11 schemas hard-coded in `components/meta_registry.h`), `MeshRenderer` carries
the bgfx renderer's registry ids, and a component the serializer does not know
is not preserved.

**The format, modelled on glTF's core plus extensions:**
1. **Components store intent, never implementation.** Plain data; assets by
   UUID; no GPU handle, body pointer or provider object in anything saved.
2. **A schema registry.** Whoever owns a component (core or a provider)
   registers its name, version (`kAbiRevision`) and fields; one generic
   serializer reads and writes all of them (`scene/reflected_serde.h` is the
   start).
3. **A common vocabulary, owned by core:** Transform, Name, Camera, Light,
   MeshInstance, Sprite, Material, RigidBody and Collider (as intent),
   AudioSource. **Material is data**: a standard PBR parameter set, texture
   references and optional extension blocks; each renderer makes its own
   shaders from it when it cooks.
4. **Provider components are namespaced**: `aaa.RayTracedShadows`,
   `jolt.SoftBody`, `voxel.Volume`. Others ignore them.
5. **Old data migrates, never breaks.** A component's `kAbiRevision` (WO-051)
   refuses old *kits*, but a scene saved under an earlier revision must still
   load: each revision bump registers a migration (old fields to new) in the
   schema registry, run at load, and tested against a scene saved before the
   bump.
6. **Unknown components are kept, never dropped.** Without the provider they
   load as opaque data and save back unchanged, so a scene survives an editor
   that lacks the provider it was made with.
7. **The project declares extensions as required or used.** A missing required
   one fails at load, by name; a missing used one is ignored with one warning.
8. **Provider settings live in the project**, namespaced (renderer quality,
   physics substeps), not on entities.

## 5. The change stream

Every provider that keeps its own copy of the world (the renderer's GPU scene,
physics bodies, audio voices, network replication) needs to know what changed
since it last looked, not to re-read everything. Core owns one change stream
per registered component type: created, changed, destroyed, with stable ids.
It is WO-019's retained-scene table and rebuild-and-diff check made general,
and it is specified and tested before any provider depends on it.

**It costs nothing for a component type nobody listens to** (DR-0011): changes
are recorded only for types with a subscriber, so an empty game records none,
and `empty_game_budget_test` checks it.

## 6. Providers that differ

The aim is to avoid the lowest common denominator: a small shared floor, and
each provider's strengths reachable explicitly.

- **A floor contract plus a conformance suite.** Every renderer draws Camera,
  Light and MeshInstance; every physics provider does rigid bodies and
  collision events. A provider that fails the suite is not a provider.
- **Capabilities reported at creation**, like librhi's feature bits. The
  editor greys out what is missing; gameplay asks
  (`physics.has("soft_bodies")`).
- **Optional interfaces found by query** (picking, batched raycasts, previews),
  as `renderer-contract.md` §3.5 does for renderers.
- **Extension components** (§4.4) carry each provider's own data.
- **A declared fallback per extension:** ignore, approximate (ray-traced
  shadows fall back to shadow maps), or refuse to load.
- **Trade-offs are visible**: capabilities, namespaced settings, and the
  profiler channel (`docs/contracts/profiler-channel.md`), never hidden.

## 6a. New kinds of provider

A provider developer is not limited to the kinds above.
- **A kind is a service: a name and a version** (`"voxelworld.v1"`,
  `"ai.navigation.v1"`), defined in whoever's headers declare it. The core
  registry knows services, never particular kinds.
- **A provider declares what it provides and what it requires.** The registry
  connects them at startup and refuses, by name, a missing service or a version
  mismatch (the kit ABI's checks, applied to services).
- **New data comes with it:** a provider registers its own components (§4,
  namespaced) and its own asset types. An asset type has two halves: a cooker
  plugin for development time and a loader for runtime, so §2a applies on its
  own: the cooker half never ships.
- **Optional extras are found by query** (§6), so a provider exposes what it
  needs without growing a contract for everyone.

## 7. Where each provider kind stands

| Kind | Chosen by name today | Contract | Gap |
|---|---|---|---|
| Physics | yes (`jolt`, `none`) | `engine-plugin.md` | takes `flecs::world` directly |
| Scripting | yes (`lua`, `none`) | `script-services.md` | |
| Audio | yes (`miniaudio`, `none`) | `audio-provider.md` | |
| Rendering | no (`runtime_boot.cpp` picks in code) | `renderer.md` (bgfx-shaped) | `renderer-contract.md` |
| Networking | no (`modules/net`, Rust) | none | a contract, when a second implementation is wanted |

**One gap shared by every kind:** an unknown provider name logs an error and
falls back to the default provider (`plugins/stock_plugins.h`), so a typo in
`project.json` runs a different provider than the one asked for. Under §6 it is
refused at startup, by name, with the list of providers this build has.

## 7a. Every tool is a CLI over the library

Every operation is a library call first, then a CLI command, and the editor
calls the same library. An agent can then do what a developer does in the
editor: edit text files (scenes and projects are JSON), run commands, read
results. That needs one `engine` command with subcommands (`new`, `import`,
`cook`, `build --platform`, `run --frames`, `screenshot`, `inspect`,
`validate`, scene edits), `--json` output, meaningful exit codes, no prompts,
and a headless run that saves screenshots. The tools that exist
(`engine_project`, `engine_cook`, `engine_build`, `engine_player --frames`)
become its subcommands.

## 8. Order of work

Each step leaves a working engine.
1. **This plan agreed**, then decision records for §3 (the world boundary) and
   §4 (the scene format).
2. **The schema registry and the generic serializer**, with unknown components
   preserved. Proof: every existing scene round-trips byte-identically.
3. **The change stream**, generalising WO-019, with its own tests.
4. **The world API** at the kit ABI, and the audit rule (§3.4). Existing kits
   keep loading until a version bump retires the old entry points.
5. **Providers moved onto it**: physics first (one file), then the renderer
   contract (`renderer-contract.md` §5).
6. **Capabilities and optional interfaces** for every provider kind, with a
   conformance suite per kind.
7. **The registry, injection and services** (§2b, §6a): providers registered by
   name and by service, unknown names refused, static linking per platform in
   `engine_build` once WO-025 lands.
8. **The development-time list checked** (§2a): the symbol check and the
   exported-folder check, in CI.
9. **The `engine` CLI** (§7a), the existing tools as its subcommands.

## 9. Open questions for the owner

1. Should the world API for library users be the C kit ABI with a C++ wrapper
   (as librhi does), or a separate C++ API? Recommended: **one C API with a C++
   wrapper**, so both users see the same world.
2. Is networking a provider kind now, or core until a second implementation
   exists? Recommended: **a provider kind, contract written when the second
   implementation is wanted.**
3. May a game load a provider that was not shipped with its engine version, as
   a binary? Recommended: **no, not in v1** (DR-0006).
