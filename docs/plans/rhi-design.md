---
status: plan
covers:
  - src/render/
---
# Replacing bgfx: a GPU-driven RHI for this engine

> **Status: proposed, nothing built.** No `verified:` date, because there is
> nothing as-built to verify. This is a design and a plan, written to be argued
> with. Every number in Part 1 was measured in this repo; every number in Part 4
> is an estimate and labelled as one.

## 0. The answer, up front

**Yes — but not for the reason in the question, and not first.**

"bgfx is a lowest common denominator over a dozen APIs, most of them legacy" is
true and it is not what is costing us anything today. The measured bottleneck in
this engine is `Render.extract` — **CPU 24.8 ms against GPU 9.11 ms at 50 000 real
props** (`src/render/issues.md` R20). An RHI rewrite does not touch that 18.8 ms
of extraction. Done in the wrong order, this project is four months of work for a
frame time that does not move, and the repo has a standing rule against exactly
that kind of change (NEON was declined on a 0.4%-of-frame measurement).

The argument that does hold is stronger than "bgfx is old":

> **The only way to stop being CPU-bound is to stop doing per-object work on the
> CPU. That requires GPU-driven submission — compute culling into indirect
> arguments, with bindless resources and no per-draw uniforms. bgfx has the
> compute and the indirect draws. It cannot give us bindless, and its per-draw
> uniform model is not something you can opt out of.**

So the project is not "a modern bgfx". It is **the substrate that lets the render
path stop scaling with entity count on the CPU**, and ray tracing, mesh shaders
and the rest arrive as consequences of the same design rather than as features
bolted on. Framed that way it is justified by our own numbers, and the phase order
below falls out of it.

## 1. What bgfx actually costs us, measured

Not opinions — findings already in the tree:

| Cost | Evidence |
|---|---|
| **Per-draw uniform payload is a hard ceiling.** ~1 KB per draw into Metal's fixed 8 MB scratch buffer **with no bounds check**, so ~8192 draws segfaults inside `commit`. `kMaxDrawsPerFrame = 4096` exists to guard a missing check in someone else's backend. | `docs/architecture/renderer-vs-production.md` |
| **No bindless.** `setTexture(stage, …)` per draw is the only model. This is what blocks GPU-driven submission, because an indirect draw cannot bind anything. | API surface |
| **No ray tracing at all.** Not exposed, not in the abstraction's vocabulary. | — |
| **No explicit barriers, queues, or timeline semaphores.** So no async compute for culling or BVH builds, no copy-queue streaming. | — |
| **Threaded submission measured WORSE.** Two of three runs stalled ~1 second on Metal drawable acquisition, and pipelining costs a frame of latency against the motion-to-photon budget this engine exists for. We deliberately run single-threaded. | `src/render/renderer/device.cpp` |
| **Resource-pool walls we pay for in engine design.** `BGFX_CONFIG_MAX_*_BUFFERS = 4096` once dropped 92% of a scene. | `src/assets/issues.md`, `tests/mesh_dedup_test.cpp` |

And what bgfx is **not** costing us, which matters just as much for honesty:

- it is not the frame's bottleneck (extraction is);
- the 4 096 draw ceiling is *ours*, guarding *their* missing check — bgfx's own
  limit is 65 535;
- 50 000 real props already submit in **299 draws** with 299 material binds, after
  submesh-granular visible sets and instancing (R18). The submission model was the
  wall, and we already moved it a long way inside bgfx;
- it has ~15 years of driver-bug workarounds we currently get for free. **This is
  the real cost of leaving, and it is not an API-design problem.**

## 2. The surface area we actually have to replace

This is the finding that makes the project tractable, and it is why I would say
yes rather than no. Counted from the tree:

- **~72 distinct `bgfx::` symbols, of which ~48 are functions.** Not a dozen
  APIs' worth of surface — one engine's worth.
- The whole list is unremarkable: create/destroy for buffers, textures, shaders,
  programs, uniforms, framebuffers; `setState`/`setTexture`/`setUniform`/
  `setVertexBuffer`/`setIndexBuffer`/`setTransform`/`submit`; views; `frame`;
  `getCaps`; `getStats`; transient and instance-data buffers.

### 2.1 The contamination, re-counted — and it is two problems, not one

**Corrected 2026-08-28.** This section previously read "57 files include bgfx, 30
of them outside `src/render/`" and treated that as one number. It is two, they
cost completely different things, and conflating them made the cleanup look four
times larger than it is.

| | files | of which |
|---|---|---|
| `#include <bgfx/…>` outside `src/render/` | **18** | 10 tests, 3 editor |
| **non-test, non-editor graphics coupling** | **5** | `asset_service.cpp`, `async_loader/upload.cpp`, `mesh_loader.cpp`, `gltf_importer.cpp`, `assimp_importer.cpp` |
| `#include <bx/…>` only — a **math** dependency | 18 | `core/math_types.h`, `core/transform.h`, `animation/pose.h`, `components/*` |

**The graphics problem is five files.** Every one of them mixes *loading* with
*GPU upload*, which is the same defect in five places and the one that has to be
fixed for a headless server, a second backend, or an embedding host — all three
want bytes handed over without a device in the room.

**The bx problem is a different project and probably not urgent.** `bx` is
bgfx's base library, and 18 files depend on it for MATH — `Vec3`, `mtxMul`. That
survives bgfx's removal untouched if we want it to; it is a maths-library choice,
not a graphics dependency, and it should be decided on its own schedule rather
than being swept into an RHI migration.

`src/runtime/docs/bgfx-includes-in-runtime.md` still names `camera_util.h` as the
poster child. **That one is already fixed**: `homogeneousDepth` is a `bool`
parameter now, with the header comment recording it as audit A.2. `runtime.h`
remains real — `Renderer m_renderer` by value at line 253, pulling bgfx
transitively through `render/primitive_library.h`, and that is what blocks a
headless build.

**The cleanup has to happen whether or not we replace bgfx** — it is the same
refactor the headless dedicated server needs, and the same one an embedding host
needs. So G1 pays for itself even if the RHI is abandoned. That is the single
most important property of this plan: **its early phases are valuable
independently of its late phases.**

### 2.2 The headroom we have never touched

Measured 2026-08-28, and it belongs in this document because it changes what
G0 should be:

> **bgfx has compute dispatch, indirect buffers, storage buffers and
> `submit(view, program, indirectHandle)`. This engine has ZERO call sites for
> any of them.**

`renderer-vs-production.md` says the same thing from the other side — "compute
shaders, indirect draws, multi-threaded encoders, instancing; we use one of those
four" — and instancing is the one.

So the claim "bgfx cannot give us GPU-driven rendering" is currently **untested**.
What bgfx genuinely lacks is **bindless**, and that is what blocks GPU-driven
rendering *for a textured game scene*, because an indirect draw cannot bind a
texture per draw. It does not block a compute cull writing indirect args, nor
per-instance data read from a storage buffer.

That distinction is worth a spike before committing a year: a GPU-driven cull →
indirect draw path on bgfx, with per-instance material indices in a storage
buffer, would either move the 24.8 ms extraction number or fail against a wall we
can name precisely. Either outcome is worth more than the plan below is without
it.

The seam we need also half-exists. `IRenderPipeline` is a real swap point with one
implementation. But `RenderContext` hands pipelines `bgfx::TextureHandle` and
`bgfx::ViewId`, so the seam leaks today and has to be closed before it can carry a
second backend.

## 3. The measurement trap, which is the sharpest point in the question

> **Corrected 2026-08-28 — this section's conclusion was wrong, and §4.1's axiom 6
> with it.** What follows about SoC measurement remains true. What did NOT survive
> is the inference drawn from it: "stop pretending the Mac is a performance
> target." **Apple Silicon is a SHIPPING target**, because the second consumer of
> this renderer — vCAD — ships on macOS and iPad, where Metal is the only backend
> that exists. See §3.1.

**"On SoCs like Apple's, one measurement gives false positives."** This is correct
and it invalidates part of our own roadmap, so it deserves to be stated plainly:

Every renderer number in this repo was measured on an M-series Mac — unified
memory, no PCIe transfer, tile-based deferred rasterisation, no discrete VRAM
budget. On that machine:

- upload cost, staging buffers and residency are close to free, so the entire
  streaming and eviction design is measured in its best case;
- bandwidth-bound passes behave unlike a discrete GPU's;
- and crucially, **GPU-driven rendering's whole payoff — moving per-object work off
  a CPU that is feeding a bus — is the thing an SoC hides.**

So: **a measurement rig on real PC hardware is required before any D3D12 or Vulkan
performance claim is falsifiable.** The testing farm (headless x86 Debian, SQLite
results DB) has no GPU lane, and this repo's entire method is falsifiability.

### 3.1 What that argument does NOT license

The original version of §3 concluded that the Mac should stop being a performance
target and that **nothing** could start before the farm had GPUs. Both halves need
correcting, and the second one has been holding up work that needs no farm at all.

**Apple Silicon is a shipping target.** This renderer has two consumers:

| | platforms | backends available |
|---|---|---|
| the game engine | Windows, Linux, macOS (editor) | D3D12, Vulkan, Metal |
| **vCAD** | **macOS and iPad** | **Metal, and nothing else** |

vCAD hosts 50 000-part assemblies on an iPad. That is the weakest CPU in the
entire picture driving the largest object count, which is precisely the workload
GPU-driven submission exists for — so the payoff is *largest* on the platform the
old §4.1 called dev-only and allowed to be slow.

**And measurement gates performance claims, not structural ones.** De-contamination
(§2.1), opaque handles, a retained scene, stable object ids — every one of those is
falsifiable today with link-time assertions and headless builds. G0 gates
G4–G8. It does not gate G1, and treating it as though it did is why the
five files in §2.1 are still uncleaned.

## 4. Design

### 4.1 Axioms

Six, and each one is a thing we refuse rather than a feature we add:

1. **No per-draw uniforms. Ever.** All per-draw data lives in GPU buffers indexed
   by draw ID. This single rule deletes the 8 MB ceiling, the `kMaxDrawsPerFrame`
   guard, the one-deep material-bind cache (`R7`), and it is what makes indirect
   draws expressible at all.
2. **Bindless-only.** Every texture and buffer receives a shader-visible 32-bit
   index at creation. There is **no binding API** — `rhi::TextureHandle` *is* the
   index the shader indexes with. D3D12: one `CBV_SRV_UAV` heap with SM 6.6
   `ResourceDescriptorHeap[]`. Vulkan: one giant descriptor set with
   `descriptor_indexing` (core 1.2) and `nonuniformEXT`. This is a *smaller* API
   than bgfx's, not a bigger one.
3. **Explicit queues and timelines.** Graphics, async compute, copy. Compute
   culling and BVH refits overlap graphics; streaming uploads go on copy.
4. **Barriers come from a render graph, never from the caller.** Passes declare
   reads and writes; the graph inserts transitions and aliases transient targets.
   Manual barriers are the #1 source of Vulkan/D3D12 correctness bugs, and fully
   automatic tracking is the thing that made bgfx's model conservative.
5. **GPU-driven is the default path, CPU-driven is the debug path.** Not the other
   way round — otherwise the fast path is the untested one, which is the drift this
   repo already refuses in extraction ("ONE body, serial or parallel").
6. **TWO backends: Metal 4 and Vulkan 1.3. D3D12 is deferred, and Xbox is its
   trigger.** *(Rewritten twice on 2026-08-28. The original read "three backends,
   only two of them ship", with Metal 3 as a dev-only backend "explicitly allowed
   to be slower and feature-reduced". The first correction made all three ship.
   This one cuts the count to two.)*

   **Coverage is why.** Between them these two reach every platform either product
   ships on, and D3D12 adds exactly one thing neither covers:

   | | Metal 4 | Vulkan 1.3 | D3D12 |
   |---|---|---|---|
   | macOS, iPadOS, iOS, visionOS | ✅ | MoltenVK only | — |
   | Windows | — | ✅ | ✅ |
   | Linux, Steam Deck / Proton | — | ✅ | — |
   | Android | — | ✅ | — |
   | **Xbox** | — | — | **✅ only** |

   **Complexity is the decisive argument, not coverage.** §9 already names the
   dominant unschedulable risk of this whole project: the ~15 years of driver
   quirks bgfx absorbs for us, which we rediscover one vendor at a time. A third
   backend multiplies that risk, the validation setups and the CI legs — forever,
   for one developer, to reach a platform neither product ships on today.

   **Metal 4 is what makes two backends sufficient rather than a compromise.**
   The earlier argument for pairing Metal with D3D12 was that they disagree most,
   so an abstraction satisfying both is unlikely to be secretly shaped like
   either. That was reasoning about **Metal 3**, which tracked resources
   implicitly. Metal 4 is an explicit API: resources are **untracked by default**
   and need explicit barriers, `MTL4ArgumentTable` replaces per-resource binding
   (this is the bindless model of axiom 2), residency sets make resources resident
   with minimal per-frame CPU cost, and `MTL4CommandBuffer` is reusable via
   `beginCommandBuffer(allocator:)`. That is structurally Vulkan's shape. The
   axioms above translate to both without either being bent.

   Enough divergence remains — residency sets versus memory heaps, argument tables
   versus descriptor sets, the queue and submission models — that an abstraction
   satisfying both is unlikely to be a thin veneer over one of them. That is the
   property that keeps D3D12 *later a backend rather than a redesign*.

   **The bet, stated so it is a decision:** Vulkan-on-Windows is not D3D12, which
   is Windows' native and generally best-tested path, especially on Intel iGPUs.
   It is a good bet — every IHV ships Vulkan on Windows and Proton has hardened it
   enormously — but it is a bet, and the fallback is adding the third backend.

### 4.2 The core API, concretely

```
rhi::Device                 // adapter, queues, feature bits
rhi::Buffer / Texture       // handle IS the bindless index; no views in the API
rhi::Pipeline               // graphics | compute | raytracing, from cooked blobs
rhi::CommandList            // records; never allocates; thread-affine
rhi::TimelineValue          // submit returns one; wait on it, don't fence-juggle
rhi::RenderGraph            // passes, declared resources, derived barriers
rhi::BufferWriter           // ring-allocated upload scratch, frame-scoped
```

Notably **absent**, and deliberately: `setUniform`, `setTexture`, `setState`,
`setTransform`, view IDs. State lives in a pipeline object baked at cook time.
Per-object data lives in buffers. Draw order is the graph plus the sort key we
already build in `rworld::`.

### 4.3 What the frame looks like

```
extract (CPU)      persistent instance buffer, only DIRTY entities re-uploaded
  ↓ copy queue
cull (compute)     frustum + HZB occlusion over the instance buffer
  ↓                → compacted indirect args + per-draw index buffer
draw (indirect)    ExecuteIndirect / vkCmdDrawIndexedIndirectCount, bindless
  ↓
RT passes          inline ray queries for shadows / AO / gameplay visibility
```

The load-bearing consequence: **the CPU stops touching per-visible-object data.**
Extraction becomes an incremental upload of what changed, not a rebuild of
everything — which is the fix for the 18.8 ms that this whole document started
from, and it is not reachable through bgfx.

### 4.4 What we keep

More than one might expect, and this is the argument for the migration being
survivable:

- **`rworld::` stays, and stays GPU-free.** Sort keys, LOD selection, light
  packing, frustum math are pure functions over PODs. LOD selection in particular
  is *already* the right shape to run per-instance in a compute shader.
- **The whole asset and cook layer stays.** The DDC, content-addressed cooking,
  the `.cooked` formats, LOD decimation (v5, this week). Only the shader cooker's
  back end changes.
- **`rdiag::SubmitStats` stays and gets more important** (see §6).
- **`IRenderPipeline` stays** as the A/B seam.

### 4.5 Ray tracing, specifically

Worth designing now because it constrains the resource model, not because it is
early:

- **Inline ray queries first** (`DXR 1.1` / `VK_KHR_ray_query`) — no separate
  shader stages, no shader-binding-table machinery. Cheapest useful path: RT
  shadows (which sidesteps the cascaded-shadow-map project entirely), RTAO.
- **Non-obvious payoff for an FPS: gameplay queries.** Line-of-sight, AI
  visibility and audio occlusion against the same BVH the renderer uses, on the
  async compute queue. That is a genuine differentiator and it costs almost nothing
  once the TLAS exists.
- **What it will actually cost:** BLAS per LOD level (interacts directly with this
  week's decimation work), per-frame BLAS refit for skinned meshes, TLAS rebuild
  per frame, and a real VRAM line item. Alpha-tested geometry needs any-hit
  shaders, which is the same problem as our current "alpha-tested casters cast
  solid shadows" gap (R19), solved properly instead of worked around.

## 5. The shader toolchain — the hidden 40% of the project

This is the part that sinks these projects, so it goes in the plan rather than
being discovered in month three. bgfx gives us a shader language *and* `shaderc`
*and* reflection. Replacing it means owning:

- **HLSL 2021 / SM 6.6+ as the one source language.** DXC → DXIL for D3D12, DXC →
  SPIR-V for Vulkan, and SPIR-V → MSL (via `spirv-cross` or Metal Shader
  Converter) for the dev backend.
- **Reflection**, though far less of it: bindless means there is almost no binding
  surface left to reflect. We already have `shader_reflect.h`.
- **Variants/permutations**, which we already have machinery for.
- Rewriting every existing `.sc` shader in HLSL. There are few, which is lucky.

The good news is structural: `shaderc_invoke.cpp` already shells out to an
external compiler with per-profile host gating (a D3D profile is correctly refused
on a macOS host). Swapping the subprocess for DXC is a contained change to one
cooker, and the DDC already keys cooked output on cooker version — so the whole
shader cache invalidates itself correctly the day we switch.

## 6. The cost nobody mentions: this weakens our quality story

I want this on the record, because it is the strongest argument *against* the
project and it is specific to this repo.

This engine's trustworthiness rests on two properties:

1. **`rworld::` is GPU-free**, so culling, sort order and LOD selection have tests
   that can fail without a device.
2. **`rdiag::SubmitStats` counts on our side of the API**, because bgfx's Noop
   backend never sets `numDraw` — which is what made `render_pipeline_test`
   possible at all.

**GPU-driven rendering moves both of those onto the GPU.** When the cull is a
compute shader, `render_world_test` no longer tests the cull that ships; when draws
come from an indirect argument buffer the GPU wrote, `SubmitStats` cannot count
them. We would be trading a subsystem with mutation-checked headless tests for one
whose decisions are invisible to the test suite.

That is survivable, but only deliberately:

- **Readback-based assertions.** The compute cull writes its survivor count and
  compacted args to a buffer we read back in a validation mode and compare against
  `rworld::buildVisibleSet` on the same frame. Axiom 5's CPU path is not
  vestigial — it is the oracle.
- **GPU-side asserts** (a debug buffer shaders append to) surfaced through the
  existing `Renderer` log tag.
- **The farm needs real GPUs** — §3 again, from the other direction. Golden-image
  tests also finally become possible, which is the gap keeping `src/render` at
  `tier: working` today.

Net: the tier for the new path starts at `prototype` and has to earn its way back.
Pretending otherwise is how this becomes a rewrite that is "done" and untrusted.

## 7. Migration: strangle, never rewrite

The seam exists; use it.

1. **Close the seam properly.** Purge bgfx from the 30 files outside
   `src/render/`, and get `bgfx::TextureHandle`/`ViewId` out of `RenderContext`
   behind opaque engine handles. *This is the headless-server refactor too.*
2. **`modules/rhi/`** as a sibling of `modules/assetlib/` — its own library, its
   own tests, no engine dependencies.
3. **`GpuDrivenPipeline : IRenderPipeline`** on `rhi`, selected by a project flag.
   `ForwardPipeline` on bgfx keeps working the entire time.
4. **A/B on the farm** — same scene, same seeds (`gen_fuzz_scene.py`), submit
   counters and GPU time side by side. This is how SDL3-vs-GLFW and
   scalar-vs-NEON were decided in this repo; it is the house method.
5. **Delete bgfx only when the new path wins on real hardware**, on a real scene,
   with the tests green.

At no point is `main` broken, and at every point the thing can be abandoned with
the cleanup work already banked.

## 8. Phases, with exit criteria

> **These are the `G` sequence.** Three renderer documents run phase sequences and
> all three used to number from 1 unprefixed, so "Phase 4" meant this document's
> forward-pipeline port, `renderer-program.md`'s render graph, AND
> `renderer-audit-and-plan.md`'s submission-efficiency work, which is **already
> done**. `A` is the audit's, `P` is the programme's, `G` is this document's; `R`
> is reserved for the audit's finding ids. `renderer-program.md` §4 carries the
> P↔G mapping and is the document that decides ORDER — this one decides content.

Each phase must be independently defensible — no phase justified only by the next.

| # | Phase | Exit criterion |
|---|---|---|
| **G0a** | **The bgfx GPU-driven spike** (§2.2): compute cull → indirect args → indirect draw, per-instance data from a storage buffer, on the 50 k fuzz scene. No new backend. | Either extraction stops dominating, or we can name the exact wall. **Weeks, not months, and it decides whether G2–G6 are worth a year.** |
| **G0b** | **GPU measurement lane on the farm** (one NVIDIA + one AMD box, D3D12 + Vulkan, GPU timings into the SQLite results DB) | The current bgfx renderer's numbers reproduced on discrete hardware. **We will learn things here that change the rest of this plan.** Gates G4–G8; does NOT gate G1 (§3.1). |
| **G1** | **De-contaminate.** bgfx out of the 30 non-render files, out of `RenderContext`; headless server builds with no graphics libs | `engine_runtime` links without bgfx for a server target; 64/64 tests green |
| **G2** | **`rhi` core**: device, queues, timelines, buffers, textures, bindless heap, command lists. D3D12 + Vulkan. Triangle. | One triangle, both backends, under validation layers with zero warnings |
| **G3** | **Shader toolchain**: HLSL → DXC → DXIL/SPIR-V through the existing cooker and DDC | Every current shader cooks and renders on both backends |
| **G4** | **Port the forward pipeline 1:1**, CPU-driven, but *no per-draw uniforms* — per-draw data in a structured buffer indexed by draw ID | Pixel-comparable to bgfx on the fuzz scene, **and the 4 096 draw ceiling is gone**. First point where we beat bgfx on something real. |
| **G5** | **Render graph**: declared passes, derived barriers, transient aliasing | Shadow + opaque + a post pass through the graph; peak VRAM measurably lower than the hand-managed version |
| **G6** | **GPU-driven**: compute cull → HZB occlusion → indirect draws; incremental instance upload | `Render.extract` no longer scales with entity count. **This is the phase the whole document is for.** |
| **G7** | **Inline ray tracing**: BVH management, RT shadows, RTAO, gameplay ray queries | RT shadows replace the shadow map on the fuzz scene at equal or better cost |
| **G8** | **Mesh shaders / cluster LOD** — where this week's decimation graduates toward Nanite-style clusters | Dense geometry stops costing draws at all |

## 9. Effort, honestly

Estimates, explicitly labelled as such, for one engineer working with AI at the
pace this repo has actually moved:

| Phases | Estimate |
|---|---|
| 0–1 (rig + de-contamination) | **3–5 weeks**, and worth doing regardless |
| 2–4 (parity, no per-draw uniforms) | **3–4 months** |
| 5–6 (graph + GPU-driven — the payoff) | **2–4 months** |
| 7–8 (RT, mesh shaders) | **3–6 months**, and open-ended |

So roughly **a year to the full vision, ~6 months to the point where it is
demonstrably better than what we have.** The dominant risk is not API design — it
is the 15 years of driver quirks bgfx absorbs for us, which we will rediscover one
vendor at a time and cannot schedule.

## 10. What would make me say stop

- G0 shows the GPU is nowhere near the limit on discrete hardware either.
  Then the answer is incremental extraction and job-system work, not an RHI.
- G4 lands and the win is only the draw ceiling. That is a real but modest
  prize for three months; reassess before G5.
- The shader toolchain (G3) slips past ~6 weeks. That is the classic sinkhole
  and the honest signal to reconsider scope.

## 11. Decisions needed before any code

*Revised 2026-08-28. Decisions 1 and 2 were answered by facts that arrived after
this document was written, not by argument.*

1. ~~**Which two backends is the API designed against?**~~ **Answered: Metal 4 and
   Vulkan 1.3, and there is no third for now.** See §4.1 axiom 6. D3D12's trigger
   is Xbox, or a Windows Vulkan driver problem that actually shows up rather than
   one we anticipated.
2. ~~**Metal's status** — dev-only?~~ **Answered: no.** Metal 4 is a first-class
   shipping backend and one of the two the API is designed against. See §3.1.
3. **Minimum spec — and this is where two of our own documents contradict each
   other.** *(Raised 2026-08-28.)*

   > `renderer-architecture.md` §1.3: "The low-end floor is real hardware. **Intel
   > UHD 630**, ~128 MB usable VRAM, 4 GB system RAM. **Not a stretch goal — the
   > acceptance test.**"
   >
   > This document, previously: "SM 6.6 / `ResourceDescriptorHeap` and mesh shaders
   > mean roughly **Turing+/RDNA2+**."

   UHD 630 is Gen9.5, 2017. Turing is 2018. **Both cannot be true**, and the
   clustered-forward decision in `renderer-architecture.md` §2 — the reason this
   engine is not deferred — was justified entirely by that 128 MB floor. Nobody had
   written the conflict down.

   It resolves, but only as a TIERED spec rather than one number:

   | capability | UHD 630 (Gen9.5) | needed by |
   |---|---|---|
   | Vulkan 1.3, compute, indirect draw | ✅ | G4–G6 |
   | Descriptor indexing / bindless | ✅ core in 1.2, tighter limits | G6 |
   | Mesh shaders | ❌ | G8 |
   | Hardware ray tracing | ❌ | G7 |

   So **GPU-driven cull → indirect → bindless reaches the stated floor** and the
   floor survives. G7 and G8 do not, and must therefore be explicitly OPTIONAL
   tiers with a working path when absent — not the baseline this document assumed.
   Ray tracing is optional by the same reasoning.

   The iPad floor is a third number, set by argument buffers and
   `MTLIndirectCommandBuffer`, and it has to be stated before G7 rather than
   discovered in it.
4. **G0b hardware** — one NVIDIA + one AMD box on the farm. Every *performance*
   claim after G4 needs them; G1 does not (§3.1).
5. **Do we do incremental extraction first?** It is cheap, independent, and attacks
   the *actual* current bottleneck. Recommendation: yes, in parallel with
   G0–G1, so the frame gets faster while the substrate is being built.
6. **NEW — does the RHI compile shaders?** Recommend **no**: it takes bytes
   (DXIL/SPIR-V/metallib) and the cooker stays host-side. That is NVRHI's choice
   and it is what keeps a second consumer from inheriting our content pipeline. It
   also contains §5, which this document calls the hidden 40%.
7. **NEW — bindless-only, or binding sets?** Axiom 2 says bindless-only, and
   GPU-driven at 50 k objects genuinely needs it. Worth recording that every
   reusable RHI shipping today (NVRHI, and NRI's higher-level tier) chose immutable
   binding sets instead, explicitly for validation. Recommend keeping axiom 2 and
   accepting we take the harder validation story — but knowingly.
