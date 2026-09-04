---
status: plan
covers:
  - src/
---
# Where this engine stands against a AAA one

> **What this document is.** A pillar-by-pillar comparison against what a
> shipping AAA engine has, with an evidence line for every claim and no adjective
> that is not backed by one. Written 2026-09-01.
>
> **What it is not.** A roadmap. [`../process/roadmap.md`](../process/roadmap.md)
> and [`renderer-program.md`](renderer-program.md) decide order. This decides
> *what is true*, so those two have something honest to order.

## 0. The finding, in one paragraph

**This engine has built the expensive half and skipped the visible half.**

The expensive half is the set of things that cannot be retrofitted: a frozen
plugin ABI, a content-addressed cook pipeline, a memory discipline, a test
contract enforced by a machine. The visible half is content-facing surface area —
particles, post-processing, terrain, streaming, replication — which is enormous
in volume and almost entirely *additive* on top of a foundation that will not
need to be rebuilt.

Almost every independent engine is the exact inverse, and that inversion is why
they die: by the time the foundation is needed, everything is already leaning on
its absence.

**The one exception, and it is the important one:** the renderer gap is *not*
additive. See §3.

## 1. The eight pillars

Nobody ships all eight well. What makes an engine AAA is that none of them is
*missing*.

| # | Pillar | This engine | Evidence |
|---|---|---|---|
| 1 | Module / extension architecture | **above the line** | §2.1 |
| 2 | Asset pipeline | **above the line** | §2.2 |
| 3 | Foundation (memory, jobs, ECS) | **above the line** | §2.3 |
| 4 | Engineering process | **above the line** | §2.4 |
| 5 | Renderer | **working, not AAA** | §3 |
| 6 | Animation | **working, not AAA** | §4.1 |
| 7 | Simulation services (physics, audio, nav, script) | **present, not deep** | §4.2 |
| 8 | Content-facing systems | **absent** | §5 |

## 2. Where this engine is at or above the line

### 2.1 Module architecture and the extension model

This is the strongest pillar, and it is genuinely above most commercial engines.

**Unreal has no stable plugin ABI at all** — plugins are recompiled against the
engine, per version, indefinitely. Unity's extension surface is C#, so the
question does not arise natively.

What exists here:

| claim | evidence |
|---|---|
| Frozen offsets, append-only growth; a shipped group can never change size or position | [`../architecture/extension-model.md`](../architecture/extension-model.md) |
| A five-condition load gauntlet — `structSize`, `apiVersion`, `abiFingerprint`, `componentLayoutHash`, kit-to-kit contracts | `src/runtime/module_loader.h` |
| Ten fixture modules, each failing exactly one condition | `tests/fixtures/abi_gate_module.cpp` (`-DABI_GATE_DEFECT`) |
| Four extension tiers with a written rule for choosing between them — Plugin, Kit, Add-on, Provider | [`../architecture/extension-model.md`](../architecture/extension-model.md) |
| The out-of-process Add-on protocol: stdout/stderr are human channels, the result file is the machine channel, exit status says whether the tool *ran* — never what it decided | `include/engine/addon_protocol.h` |
| A Provider tier where a Rust conformance suite, not a code review, decides what counts as an implementation | [`../guides/audio-provider.md`](../guides/audio-provider.md), `tests/audio_provider/conformance.rs` |
| The UI seam proven to admit a non-ImGui backend, so an editor rewrite breaks no kit | `tests/ui_backend_test.cpp` |

**A negative test suite is the part that is rare.** Ten modules that each fail one
gate is what separates an ABI from a comment; the repo's own rule is that *a gate
which has never executed under test is a comment*.

### 2.2 Asset pipeline

A real pipeline, not a loader.

| claim | evidence |
|---|---|
| Content-addressed cooking with a DDC and dependency invalidation | [`../architecture/asset-cook-architecture.md`](../architecture/asset-cook-architecture.md) |
| Per-format cookers: mesh, texture, material, shader, scene | `src/assets/cookers/` |
| Out-of-process cook worker | `src/tools/engine_cook_worker.cpp` |
| Blob digest verified before third-party code is handed the bytes | v6 mesh blob integrity — ozz only ever sees bytes whose digest matched |
| Cooked-output closure checked by test | `tests/package_closure_test.cpp` |
| Fuzzed, because it eats untrusted bytes — required at `hardened` by the ladder | `fuzz_mesh_loader_test.cpp`, `fuzz_ddc_manifest_test.cpp`, `fuzz_scene_loader_test.cpp` |

`src/assets`, `src/animation` and `modules/assetlib` are all `hardened`
(`ENGINE_STATUS.md`).

### 2.3 Foundation

| claim | evidence |
|---|---|
| TLSF with tagged heaps and a per-tag census | `src/core/memory/`, `MemoryChannel` |
| Vendored libraries audited for their own `malloc` — Recast/Detour routed through the engine allocator | `src/runtime/services/nav_service.cpp`, `mem::Tag::Nav` |
| A syscall-level metric, not just a byte count | `mem::mapEventCount()` |
| Job system with slots reserved for threads the engine does not own | `kExternalThreadSlots = 8`, `src/runtime/jobs/` |
| Simulation purity enforced by a check, not a convention | `tests/sim_purity_check.cpp` |
| Endurance lanes that actually exist | `stress_swarm`, `stress_churn`, `soak_engine` |

Nine of twenty-two subsystems are `hardened`; none is `production`, and the
ladder's own rule for that is honest — `production` requires every CI platform
plus a perf claim backed by a test.

### 2.4 Engineering process

The pillar that makes the other seven durable, and the least common of all.

- **Tier claims are checked mechanically.** `engine_doctor.py check` refuses a
  tier whose evidence is not in the tree.
- **Doc staleness is derived from git**, not from a human remembering.
- **One file per defect**, with a schema, and pinned to the test that keeps it
  fixed — 48 records in [`../process/bugs/`](../process/bugs/).
- **Mutation testing is the standard for a fix**: reintroduce the bug, watch the
  test fail, restore. An untested fix is a hypothesis.
- **Numbers are either backed by a test or dated and attributed to a machine.**

The measurable effect: this session's work found four defects that existed only as
prose, and refuted half of its own thread-QoS hypothesis by testing it. A process
that can embarrass its author is a working process.

## 3. The renderer — the one gap that is not additive

`src/render` is `tier: working`, correctly.

**What exists:** forward pipeline, parallel frustum cull over archetype chunks,
discrete cooker-generated LODs, stable radix draw sort, light packing, instancing,
one shadow pass. 50 000 real props submit in **299 draws**.

**What a production renderer has that this does not**, verbatim from the
feature-by-feature table in
[`../architecture/renderer-vs-production.md`](../architecture/renderer-vs-production.md):

| dimension | here | production |
|---|---|---|
| submission | one `submit()` per draw, per-draw uniforms | indirect draws from GPU-built argument buffers, bindless |
| culling | CPU frustum spheres | GPU compute cull → compacted indirect args, HZB occlusion |
| pass organisation | hardcoded shadow + opaque | render graph with automatic barriers and resource aliasing |
| shadows | **one 2048² map, one caster** | cascades, cached static shadows, filtering |
| transparency | **none** (the sort key has the layout; nothing feeds it) | sorted, OIT, or depth-peeled |
| post | **none** | HDR chain, TAA, bloom, tonemap |

**Why this one is not additive.** Three of those rows change the *shape* of the
extract→submit path rather than adding a stage to it:

1. GPU-driven submission makes extraction an incremental upload of what changed
   rather than a rebuild — a retained scene with an ownership model
   ([`../rhi/design-api.md`](../rhi/design-api.md) §4.3).
2. Every temporal upscaler needs **motion vectors and a jittered projection**,
   which do not exist, and which constrain what the render graph must express
   ([`resource-policy.md`](resource-policy.md) §5).
3. GPU-driven rendering moves the cull and the draw count *off* the CPU, where
   the current headless tests can see them
   ([`../rhi/testability.md`](../rhi/testability.md)).

So the renderer is not "add bloom, then add shadows". It is a substrate decision
that everything downstream inherits, which is why it has its own programme
([`renderer-program.md`](renderer-program.md)) and its own directory
([`../rhi/`](../rhi/)) rather than a backlog entry.

**The measured bottleneck is not the renderer's feature set anyway.**
`Render.extract` costs **24.8 ms CPU against 9.11 ms GPU at 50 000 props**
(`src/render/issues.md` R20). Adding post-processing to that frame would be
decorating a CPU stall.

## 4. Present, but not deep

### 4.1 Animation

`hardened`, with a fuzz lane — ozz integration, clips, skeletons, skin palettes,
cooked skin format, an animator system.

Missing the AAA layer: **blend trees and state machines authored as data**, IK,
root motion, additive layers, retargeting. `src/systems/` contains exactly one
system (`animator_system.h`), which is the honest measure of how much gameplay
animation logic exists.

### 4.2 Simulation services

| service | state | evidence |
|---|---|---|
| Physics | Jolt, behind a plugin with a null implementation | `src/plugins/jolt_plugin.h`, `null_physics_plugin.h` |
| Audio | provider ABI with occlusion in the interface, miniaudio reference impl, Rust conformance suite | `src/audio/`, `include/engine/engine_audio_provider.h` |
| Navigation | Recast/Detour, allocator-routed | `src/runtime/services/nav_service.cpp` |
| Scripting | Lua, behind a plugin with a null implementation | `src/plugins/lua_script_plugin.h` |

All four are *present and swappable*, which is the architecturally hard part.
None is deep: no character controller, no vehicle model, no destruction, no DSP
graph authoring, no navmesh streaming.

## 5. Absent

Stated flatly, because a gap analysis that hedges is useless.

| Pillar | State | Consequence |
|---|---|---|
| **Networking / replication** | `modules/net` is `prototype`, last touched 2026-07-31 | No replication, prediction, rollback or authority model. The largest checklist hole after the renderer. |
| **World streaming** | none | A residency *budget* exists (`meshBudgetMB`); a streaming *system* does not. No volumes, no world partition, no LOD-driven load/unload. |
| **Particles / VFX** | none | Named in a comment in `render/renderer.h`; that is the entire footprint. |
| **Terrain, vegetation, decals** | none | — |
| **Localization** | none | — |
| **Crash reporting / telemetry** | design only | [`future-plans/crash-reporting.md`](future-plans/crash-reporting.md) |
| **Global illumination** | none | Lumen/Enlighten-class is a multi-year pillar on its own. |
| **Console platforms** | none | Gated by NDA'd SDKs — a business gate, not an engineering one. Also the trigger for a D3D12 backend ([`../rhi/design-axioms.md`](../rhi/design-axioms.md) axiom 6). |

## 6. What blocks what

The only ordering claim this document makes. Everything else is
[`renderer-program.md`](renderer-program.md)'s call.

```
G1 de-contamination ──┬─→ headless dedicated server
                      ├─→ platform embedder (host without a device)
                      └─→ any second graphics backend

GPU-driven submission ──┬─→ upscaling (needs motion vectors + jitter)
                        ├─→ virtualised geometry / cluster LOD
                        └─→ 50k-object CAD viewport on iPad

render graph ──┬─→ post chain (HDR, TAA, bloom, tonemap)
               └─→ transparency
```

Two observations fall out:

**G1 is the highest-leverage item in the whole document.** De-contaminating the
five files that mix loading with GPU upload
([`../rhi/evidence-coupling.md`](../rhi/evidence-coupling.md) §2.1) unblocks the
dedicated server, the embedder and the RHI at once — and it is valuable even if
the RHI is abandoned.

**Nothing in §5 blocks anything in §2–4.** Particles, terrain, localization and
streaming are all additive. They are large, but they are not *ordering
constraints*, and treating them as urgent would be the mistake this engine has so
far avoided.

## 7. The honest summary

Judged as a product, this engine is not close to AAA — six of the eight pillars
are incomplete and one is absent entirely.

Judged as a *foundation*, it is closer in kind to Frostbite or Decima than to an
independent engine, because the properties it has are the ones that are
prohibitively expensive to add later:

> An ABI cannot be frozen after plugins exist. A cook pipeline cannot be inserted
> under a codebase that already loads FBX at runtime. A memory discipline cannot
> be imposed on a tree where forty call sites already `new`. A test contract
> cannot be applied retroactively to claims nobody recorded.

Those are one-way doors, and this engine went through all four in the right
direction while the cost was low. Bloom is a two-way door.
