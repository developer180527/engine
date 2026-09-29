---
status: plan
---
# Subsystem contracts — survey

> **Status: plan.** A survey of *how* each subsystem is reached by the others —
> through a contract (an interface, a C ABI, a data type) or through a concrete
> class — and what it would take for subsystems to be developed independently.
> Derived from the tree on 2026-09-27 with the same include-graph walk as
> `scripts/engine_audit.py` (areas split finer: `runtime/*` and `render/*` sub-
> areas separately). Companion to `subsystem-audit.md`, which ranks the same
> subsystems for *hardening*; this one is about *boundaries*.

## 0. The goal, and what "contract" means here

The goal: any subsystem can be built, replaced or delayed **without editing its
callers**, because callers depend on a stable contract rather than on the
implementation. A caller of an unfinished subsystem still compiles and runs; it
gets a defined "nothing" and says so.

A contract is **the signature plus the semantics**, and the semantics are the
half that breaks independent work:

| part | the question it answers |
|---|---|
| shape | functions, types, handles |
| meaning of "nothing" | not ready yet? not supported here? failed? |
| ownership | who frees it; how long a pointer stays valid |
| threading | which thread may call; can it block |
| timing | synchronous, or a handle/job you poll or wait on |
| errors | return value, log, poisoned state |

Three kinds of "not done" behind a contract, and they are not interchangeable:

- **Null object** — does nothing, correctly, forever (`NullRenderer`). For
  "nothing" as a legitimate answer (a server has no GPU).
- **Stub** — "not implemented", *loudly* (logs once, returns a status). For
  work in progress, so silence is never mistaken for success.
- **Fake** — plausible behaviour for tests (fixed contacts, a canned asset).
  What lets a subsystem be *tested* before the one it calls exists.

And one enforcement tool: a **contract test** — one suite every implementation
must pass (null, fake, real). Without it two implementations drift and
"independent" becomes "incompatible".

## 1. What already exists — the pattern works here

The engine has the pattern in several places, independently arrived at, and
every one of them already paid for itself:

| contract | shape | implementations | null / fake | contract test |
|---|---|---|---|---|
| **Kit ABI** (`include/engine/engine_api*.h`, `contract.h`) | C ABI, frozen, versioned groups; kit component contracts versioned + layout-folded, refused at load on skew | engine | — | ✅ `api_abi_compat_test`, `component_abi_test`, `module_abi/` |
| **Audio provider** (`engine_audio_provider.h`) | C ABI, host services passed in | miniaudio | — | ✅ `audio_conformance` (Rust suite against the ABI), `audio_abi_check.c` |
| **Renderer** (`render/renderer_interface.h`) | `IRenderer`, 26 methods | `Renderer`, `NullRenderer` | ✅ null | ✅ `null_renderer_test` (null only) |
| **GPU seam** (`render/gpu.h`) | free functions + handles | bgfx, null | ✅ `gpu_null.cpp` | ✅ `gpu_seam_test` |
| **Platform** (`runtime/platform/platform.h`) | `IPlatform`, `IToolWindow`, UI input — defaults are "no" | GLFW, SDL3, Headless | ✅ `HeadlessPlatform`, default methods | ❌ none shared |
| **Engine providers** (`plugins/stock_plugins.h`) | `IEnginePlugin`, chosen by `project.json` | Jolt / null physics, Lua / null scripting, audio | ✅ `NullPhysicsPlugin`, `NullScriptPlugin` | 🟡 `providers_test` (swap seam, not behaviour) |
| **Script services** (`runtime/scripting/script_services.h`) | `IPhysicsService`, `IAnimService`, … | backend registers one | ✅ *null until registered, calls no-op* | ❌ |
| **Render pipeline** (`render/render_pipeline.h`) | `IRenderPipeline` extension point | forward | — | ❌ |
| **Importers / cookers** | `MeshImporter`, `ICooker` | glTF, Assimp; per-type cookers | — | 🟡 fuzz per format |
| **Input sources** | `IInputSource` | HID, window, replay | ✅ replay is a fake | 🟡 replay/determinism tests |

`script_services.h` states the idea almost verbatim: *"Declared now so the
scripting contract is stable … the calls safely no-op until the backing service
exists, and 'just work' once it does — no contract change."* That is the model
to generalise; it exists in one file.

**The biggest proven win** is `NullRenderer`: it replaced scattered
`if (!m_headless)` guards, one of which had already shipped a ~480 KB/s leak on
dedicated servers. A contract with a null object removed a *class* of bug.

## 2. The map — how every subsystem is reached

Fan-in is the number of other areas that include it (the blast radius of
changing it without a contract).

| area | fan-in | reached through | contract? | null/fake | contract test | notes |
|---|---:|---|---|---|---|---|
| `core` | 21 | free functions, value types (logger, mem, handle, transform) | 🟡 de facto | — | ✅ many unit | foundation; stable by use, not by declaration |
| `components` | 12 | **ECS data types** | 🟡 data contract | — | 🟡 layout hash for kit-visible ones only | the real inter-system contract (§3.1) |
| `render` | 11 | **concrete resource types** (`mesh.h`, `texture.h`, `material.h`, `vertex.h`, registries) *and* `IRenderer` | 🟡 split | ✅ renderer only | 🟡 | fan-in is mostly *resource data*, not rendering (§3.4) |
| `animation` | 8 | concrete registries (`skeleton_registry`, `clip_registry`, `clip_library`) | ❌ | ❌ | ❌ | |
| `runtime/jobs` | 8 | free functions (`jobs.h`) | 🟡 de facto | ❌ (no inline/serial impl) | ❌ | a serial implementation would be a free fake |
| `runtime` | 7 | `EngineRuntime`, `RuntimeContext` | ❌ concrete | — | — | hub: depends on **14** areas (§3.3) |
| `runtime/platform` | 6 | `IPlatform`, `wsi::` | ✅ | ✅ | ❌ | |
| `runtime/services` | 6 | **concrete** `AssetService`, `SceneService`, `AnimService`, `NavService`, `LutLibrary`, `AsyncLoader` | ❌ | ❌ | ❌ | the widest concrete surface (§3.2) |
| `project` | 6 | `ProjectContext` struct | 🟡 data | — | 🟡 | plain data; fine as is |
| `runtime/input` | 5 | `InputManager`, `InputSystem`/`InputMap` **singletons** | 🟡 | ✅ replay | 🟡 | singletons are hard to fake per test |
| `render/world` | 5 | pure functions | ✅ (pure) | n/a | ✅ | GPU-free by rule (LAYER-04) |
| `assets` | 5 | `ImporterRegistry`, `AssetStorage`, importers | 🟡 | ❌ | ✅ fuzz | |
| `scene` | 3 | `SceneSerializer` statics | ❌ | ❌ | ✅ fuzz | |
| `plugins` | 3 | `IEnginePlugin` + concrete plugin types (`JoltPlugin` via `dynamic_cast` in the editor) | 🟡 | ✅ | 🟡 | |
| `runtime/scripting` | 3 | `ScriptHost`, services | 🟡 | ✅ services null | ❌ | depends on **10** areas |
| `audio` | 1 | C ABI | ✅ | — | ✅ | the best-contracted subsystem in the tree |
| `systems` | 1 | concrete | ❌ | — | — | |
| `editor` | 0 | — | leaf | — | — | now split into GUI-free models + front ends (§3.6) |

## 3. Findings

### 3.1 The strongest contract in the engine is the ECS component set — declared for `Transform`, not yet for the rest

Physics reads `RigidBody`, the renderer reads `MeshRenderer`/`Light`, animation
reads `Animator`, scripts write `Transform`. Systems already cooperate mostly
through **data**, not calls — which is the architecture that makes independent
development easiest: a system can land late and its components simply sit there
unread. `components` has fan-in 12 and depends on almost nothing.

**`Transform` already has the full contract.** `runtime/transform_authority.h`
declares who owns each FIELD per entity kind (gameplay vs physics for position,
rotation, scale), names the legal way to move a physics-owned pose
(`IPhysicsService::teleport`), and runs a watcher that turns a write by the
wrong owner into a named report — pinned by `transform_authority_test` and
`physics_authority_test`. *(This section originally said no component had its
writers and readers declared; that was wrong, and was corrected 2026-09-27.)*

What is missing is the same thing for the OTHER components: who writes
`RigidBody`, `Animator`, `Light`, `MeshRenderer`, and in which phase. Beyond
`Transform`, only kit-visible components are declared at all, and only their
layout (`component_abi_test`) — the shape, not the semantics.

### 3.2 `runtime/services` is the widest concrete surface

`AssetService`, `SceneService`, `AnimService`, `NavService`, `LutLibrary` and
`AsyncLoader` are reached **as concrete classes** from `runtime`, `scripting`,
`scene`, the C++ SDK and the editor. None has an interface, a null or a fake.
This is where independent development is most blocked today: anything that
loads an asset, a scene or a clip needs the real implementation to compile
*and* to run a test.

### 3.3 `EngineRuntime` is the hub everything passes through

`runtime` depends on 14 areas. Constructing concrete implementations in
`runtime_boot.cpp` is correct — that is the **composition root**, the one place
allowed to know concrete types. The problem is that `EngineRuntime` is *also*
the API every caller reaches for (`runtime.renderer()`, `.kits()`,
`.assetLib()`, `.luts()`, `.frameArena()`, …), so a caller that needs one
service depends on all of them.

### 3.4 `render`'s fan-in is mostly resource data, not rendering

`assets`, `scene` and `runtime/services` include `render/mesh.h`,
`texture.h`, `material.h`, `vertex.h` and the registries — engine resource
*types* that happen to live in `render/`. That makes the renderer look like an
11-dependent hub, and it means "replace the renderer" (the RHI programme)
touches the asset pipeline. Moving the resource types to their own area would
leave the renderer behind `IRenderer` with a small fan-in.

### 3.5 The C++ SDK surface re-exports internals

`include/engine/engine.h`, `services.h`, `plugins.h`, `platform.h`, `render.h`
are thin umbrellas over `runtime/runtime.h`, `asset_service.h`,
`jolt_plugin.h`, `glfw_platform.h`, `render_pipeline.h` — the *implementation*
headers, with everything they include. So the C++ SDK has no contract beyond
"whatever those classes look like today". The **C ABI** is the only public
surface that is actually stable. This is fine for a pre-1.0 C++ SDK, but it
should be a stated decision.

### 3.6 The editor now demonstrates the model/front-end split

The libgui experiment (2026-09-26/27) moved every editor panel's logic into
GUI-free models (`src/editor/panels/*/model.h`, `editor/core/`), drawn by both
ImGui and libgui, each pinned by a test that fails to build if the model
includes a GUI toolkit. It is the same idea at a smaller scale: the model is
the contract, the GUI is the implementation.

### 3.7 Contract tests exist per implementation, rarely per contract

Only the audio provider and the kit ABI have a suite that any implementation
must pass. `null_renderer_test` tests the null renderer; nothing runs the same
expectations against the real one. `IPlatform` has three implementations and no
shared test.

### 3.8 There is no registry of contracts

`ENGINE_STATUS.md` tracks *directories* by tier. Nothing lists the contracts,
their state, their implementations, or their test pass rate — so the "chart of
progress where each system can land at any time" does not exist yet, and could
not be generated from the tree.

## 4. Proposed direction

### 4.1 One contract per subsystem boundary, not per class

Contracts belong where two things are developed at different speeds: platform,
renderer, GPU, physics, audio, scripting, assets/resources, scene I/O, input,
navigation, animation, jobs, the editor models. **Inside** a subsystem, plain
code. The test for "needs a contract": could someone build the other side
without reading your code?

### 4.2 A contract has a lifecycle: provisional → frozen

A contract written before anyone uses it is usually wrong. Mark new contracts
**provisional**; freeze after one or two real callers; after freezing, change
only by adding or by versioning. This reuses the tier vocabulary the repo
already has.

### 4.3 Every contract ships with a null, and a fake where tests need one

- Null: the default returned by the composition root when the real one is
  absent (`NullRenderer`, `HeadlessPlatform`, null providers — extend to
  services).
- Stub-with-status for work in progress: a "not implemented" result the
  caller can see, logged once.
- Fakes for the contracts that block tests (assets, scene, jobs-serial).

### 4.4 A machine-readable contract registry, and the chart from it

A small per-contract descriptor (name, owner area, state, implementations,
contract-test target) that `engine_doctor` reads and renders into
`ENGINE_STATUS.md` as the progress chart: each contract, its state, which
implementations exist, and how many contract tests each passes. Measured, not
self-reported — the same principle the rest of the doc system follows.

### 4.5 Suggested order (by how much independent work each unblocks)

| # | Work | Unblocks |
|---|---|---|
| 1 | **Contract registry + chart** (§4.4) and a written contract template (§0 table) — **done 2026-09-27**: `docs/contracts/`, the Contracts chart in `ENGINE_STATUS.md`, checked by `engine_doctor`, pinned by `contract_registry_test` | makes everything below visible; cheap |
| 2 | **Services contracts**: `IAssetService`, `ISceneService` (+ null + fake), callers switched to them | the widest concrete surface (§3.2); asset-dependent work becomes testable in isolation |
| 3 | **Component contracts**: per component — writer, readers, phase — on the model `transform_authority` already set for `Transform`; extend the layout hash beyond kit-visible types | the de facto inter-system contract (§3.1) becomes declared for every component |
| 4 | **Shared contract tests** for `IPlatform` and `IRenderer` (real + null run the same suite) | the two contracts with multiple implementations and no shared suite (§3.7) |
| 5 | **Resource types out of `render/`** | the renderer's fan-in drops to its real users; the RHI programme stops touching assets (§3.4) |
| 6 | **Narrow `EngineRuntime`'s accessor surface** toward per-service handles | callers depend on one service, not the hub (§3.3) |
| 7 | **Decide the C++ SDK's stability** (state that it is unstable pre-1.0, or give it contracts) | removes the ambiguity in §3.5 |

Nothing here needs to happen at once, and #1 alone changes how the rest is
tracked. Each step is independently useful.

## 5. Method, and its limits

- Edges are `#include` edges between areas, resolved with `engine_audit.py`'s
  own helpers. An include is not a use: a header can be included for one type.
  Counts are an upper bound on coupling.
- "Contract?" was judged by reading the header at the boundary: interface /
  C ABI / pure functions / data type = contract; a concrete class with private
  state reached directly = not.
- Kits, `fps_shooter` and other gitignored trees are excluded, as in the audit.
- This survey does not measure *semantic* completeness of any contract (§0's
  table) — only whether one exists. Writing those semantics down is part of
  step #1.
