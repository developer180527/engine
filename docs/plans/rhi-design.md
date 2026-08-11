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

- **~58 distinct `bgfx::` symbols, of which ~35 are functions.** Not a dozen
  APIs' worth of surface — one engine's worth.
- The whole list is unremarkable: create/destroy for buffers, textures, shaders,
  programs, uniforms, framebuffers; `setState`/`setTexture`/`setUniform`/
  `setVertexBuffer`/`setIndexBuffer`/`setTransform`/`submit`; views; `frame`;
  `getCaps`; `getStats`; transient and instance-data buffers.
- **57 files include bgfx, and 30 of them are outside `src/render/`.**

That last number is the actual problem, and it is already written up in
`src/runtime/docs/bgfx-includes-in-runtime.md`: `runtime.h`, `camera_util.h`
(includes bgfx to read one depth-convention cap!), and the async loader all speak
graphics. **That contamination has to be cleaned up whether or not we replace
bgfx** — it is the same refactor the headless dedicated server needs. So Phase 1
below pays for itself even if the RHI is later abandoned. That is the single most
important property of this plan: **its early phases are valuable independently of
its late phases.**

The seam we need also half-exists. `IRenderPipeline` is a real swap point with one
implementation. But `RenderContext` hands pipelines `bgfx::TextureHandle` and
`bgfx::ViewId`, so the seam leaks today and has to be closed before it can carry a
second backend.

## 3. The measurement trap, which is the sharpest point in the question

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

So: **the first deliverable of this project is not code, it is a measurement rig
on real PC hardware.** The testing farm (headless x86 Debian, SQLite results DB)
has no GPU lane. Until it has one, D3D12 and Vulkan work is unfalsifiable, and
this repo's entire method is falsifiability. Phase 0 exists for this reason and
nothing else.

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
6. **Three backends, and only two of them ship.** D3D12 and Vulkan 1.3 are
   shipping targets. Metal 3 is a **dev-only backend, explicitly allowed to be
   slower and feature-reduced.** This is the direct consequence of §3: we cannot
   measure the things this project exists for on Apple Silicon anyway, so we should
   stop pretending the Mac is a performance target and keep it as what it is — the
   machine the editor runs on.

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

Each phase must be independently defensible — no phase justified only by the next.

| # | Phase | Exit criterion |
|---|---|---|
| **0** | **GPU measurement lane on the farm** (one NVIDIA + one AMD box, D3D12 + Vulkan, GPU timings into the SQLite results DB) | The current bgfx renderer's numbers reproduced on discrete hardware. **We will learn things here that change the rest of this plan.** |
| **1** | **De-contaminate.** bgfx out of the 30 non-render files, out of `RenderContext`; headless server builds with no graphics libs | `engine_runtime` links without bgfx for a server target; 64/64 tests green |
| **2** | **`rhi` core**: device, queues, timelines, buffers, textures, bindless heap, command lists. D3D12 + Vulkan. Triangle. | One triangle, both backends, under validation layers with zero warnings |
| **3** | **Shader toolchain**: HLSL → DXC → DXIL/SPIR-V through the existing cooker and DDC | Every current shader cooks and renders on both backends |
| **4** | **Port the forward pipeline 1:1**, CPU-driven, but *no per-draw uniforms* — per-draw data in a structured buffer indexed by draw ID | Pixel-comparable to bgfx on the fuzz scene, **and the 4 096 draw ceiling is gone**. First point where we beat bgfx on something real. |
| **5** | **Render graph**: declared passes, derived barriers, transient aliasing | Shadow + opaque + a post pass through the graph; peak VRAM measurably lower than the hand-managed version |
| **6** | **GPU-driven**: compute cull → HZB occlusion → indirect draws; incremental instance upload | `Render.extract` no longer scales with entity count. **This is the phase the whole document is for.** |
| **7** | **Inline ray tracing**: BVH management, RT shadows, RTAO, gameplay ray queries | RT shadows replace the shadow map on the fuzz scene at equal or better cost |
| **8** | **Mesh shaders / cluster LOD** — where this week's decimation graduates toward Nanite-style clusters | Dense geometry stops costing draws at all |

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

- Phase 0 shows the GPU is nowhere near the limit on discrete hardware either.
  Then the answer is incremental extraction and job-system work, not an RHI.
- Phase 4 lands and the win is only the draw ceiling. That is a real but modest
  prize for three months; reassess before Phase 5.
- The shader toolchain (Phase 3) slips past ~6 weeks. That is the classic sinkhole
  and the honest signal to reconsider scope.

## 11. Decisions needed before any code

1. **Shipping targets** — is Windows/D3D12 first with Vulkan second, or both at
   once? (Recommend: both from day one in Phase 2, because a single-backend
   abstraction is always wrong, and Vulkan is Proton/Deck.)
2. **Metal's status** — dev-only, as argued in §4.1? (Recommend: yes, explicitly.)
3. **Minimum spec.** SM 6.6 / `ResourceDescriptorHeap` and mesh shaders mean
   roughly Turing+/RDNA2+. Ray tracing hardware, required or optional?
4. **Phase 0 hardware** — one NVIDIA + one AMD box on the farm. Nothing after this
   is measurable without them.
5. **Do we do incremental extraction first?** It is cheap, independent, and attacks
   the *actual* current bottleneck. My recommendation: yes, in parallel with
   Phases 0–1, so the frame gets faster while the substrate is being built.
