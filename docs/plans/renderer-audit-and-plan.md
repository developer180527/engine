---
status: as-built
verified: 2026-08-01
covers:
  - src/render/
---
# Renderer: Audit and Plan

> Part 1 is an **audit** — what the renderer is today, from reading the code and
> measuring it, with no proposals mixed in. Part 2 is the **plan** that follows
> from it. `docs/architecture/renderer-architecture.md` (June) already described a target
> architecture; the most important finding here is that it was never built, so
> this document supersedes its migration plan and keeps its goals.

## Part 1 — Where we actually stand

### 1.1 The pipeline as built

```
ECS world ──Renderer::buildView──> RenderView ──> ForwardPipeline::render ──> bgfx
            (src/render/renderer.cpp)   │            (src/render/forward_pipeline.h)
                                        │            culling + sorting + light
            items, lights, camera,      │            packing + material bind +
            frustum planes, target      │            submission ALL live here
```

`src/render/passes/` (shadow, sky, opaque, transparency, post, resolve) is
**scaffold — commented stubs, not in the build.** The single real pipeline is
`ForwardPipeline`, 442 lines that do everything after extraction.

The June TODOs are still in the source, verbatim: *"ECS World → Render
Extraction → RenderWorld → Visibility → Submission. Keep extraction, culling,
sorting and submission as separate stages."* The design was right; it stalled.

### 1.2 Measured (fps_shooter, 600 fixed frames, `engine_host --frames 600`)

```
draws        avg 14.7   max 19
VRAM         tex 76.0 MB   rt 71.0 MB   peak 147.1 MB
transient    vb 0.2 KB/frame   ib 0.0 KB/frame
handles      78 live (tex 10 vb 10 ib 9 fb 2 prog 14 uni 14)
CHURN        0/600 frames changed handle counts  -> STEADY
```

Read against the **target machine — Intel UHD 630, 128 MB shared VRAM, 4 GB
system RAM**, that is the headline: **a three-mesh scene at 720p already
exceeds the entire graphics budget.** Draw calls are irrelevant at this scale;
memory is the wall.

### 1.3 What is genuinely good

Worth stating plainly, because the gaps below are not a verdict on the whole:

- **Frustum culling exists** and uses real mesh bounds.
- **Draws are sorted** by mesh then material — the correct ordering for
  batching, already in place.
- **`IRenderPipeline` is a real public extension point** (`include/engine/render.h`),
  and pipelines are swappable at runtime.
- **Zero per-frame handle churn**, measured. The runtime renderer creates and
  destroys nothing in the frame loop.
- Registries reuse slots via a free list; no unbounded handle growth.

### 1.4 Findings

| # | Finding | Evidence | Severity |
|---|---|---|---|
| **R1** | **GPU resources have no identity, no refcount, no dedup.** `TextureRegistry::addTexture` is a slot allocator: every call makes a new GPU texture. Two materials referencing the same image file get two copies of it in VRAM. Nothing tracks who references a resource, so nothing can know when it is free. | `src/render/texture_registry.h` — `addTexture` appends; `removeTexture` is manual and unreferenced by any owner | **Critical** |
| **R2** ✅ | **Extraction/submission split never happened.** Culling, sorting, light packing and material binding all lived inside `ForwardPipeline`. A dev who swapped the pipeline inherited *none* of it and had to rewrite culling and sorting to draw a single triangle differently. **FIXED in Phase 3** — all four moved to `src/render/world/`; the pipeline now owns only shaders, uniforms and binds. Material *assets* remain fixed-struct (R3). | `src/render/world/`, `forward_pipeline.h` | **Critical** |
| **R3** | **Shaders are compile-time blobs.** `.sc` sources are compiled to per-platform C arrays (`vs_triangle_mtl`, `_dxbc`, `_spv`) and `#include`d. There is no shader asset, no variant system, no runtime load. A game cannot add a material type without rebuilding the engine. | `forward_pipeline.h:34–104`, `shaders/*.sc` | **Critical** (for the customization goal) |
| **R4** | **Bone palette uploaded per submesh.** `setUniform(m_uBoneMatrices, …, boneCount*4)` sits inside `bindMaterial`, which runs once per submesh. The 73-bone zombie re-uploads its whole palette for every submesh, every frame. | `forward_pipeline.h:291–293` | High |
| **R5** | **No instancing.** The sort already groups identical meshes adjacently — the setup for batching is done, the batch is not. | no `setInstanceDataBuffer` anywhere in the tree | High (at scale) |
| **R6** | **Render-target memory is ~5× the naive figure.** 1280×720 colour+depth for the scene and game framebuffers should be ≈14 MB; bgfx reports **71 MB**. Unexplained — candidates are Retina drawable scaling, D24S8 storage on Metal, and the 2-deep swap chain. | `RenderStatsChannel`, `[Renderer] Scene FB: 1280x720` | High |
| **R7** | **Redundant material binds.** `bindMaterial` re-sets every uniform and texture per submesh with no comparison against current state, even though the sort makes consecutive draws frequently share a material. | `forward_pipeline.h:277–294` | Medium |
| **R8** | **Textures are outside the residency system.** `AssetService` gained a mesh residency budget with LRU eviction; textures — the 76 MB — have none. | `asset_service.cpp` residency covers meshes only | High |
| **R9** | No LOD, no cascaded shadows (one map, one caster), no occlusion culling, no texture streaming. | absent | Medium (deferred) |

### 1.5 The root cause

R1 is upstream of most of the rest. Because resources have no identity and no
refcount:

- duplicates cannot be detected, let alone prevented;
- leaks cannot be defined, because "unused" has no meaning without references;
- **the tools you asked for cannot be written at all** — a VRAM census, a
  per-material profile and a duplicate report are all queries against a
  resource table that does not exist.

That is why the plan starts there rather than with instancing or shadows.

## Part 2 — The plan

Sequenced so each phase is independently useful and leaves a working renderer.
No big-bang rewrite; the June doc's incremental rule stands.

### Phase 1 — Resource layer (unblocks everything, including the tools)

**`GpuResourceCache`**: content-keyed, refcounted, size-aware ownership for
textures, materials and meshes.

- **Key by content**, not by call site: the asset UUID / content hash the DDC
  already computes. Requesting the same key twice returns the same handle and
  bumps a refcount — **dedup becomes structural, not a cleanup pass.**
- **Every resource records its byte size and its owner** (asset path + who
  requested it). This single table is what makes every tool below a query.
- **Refcount → eviction.** Zero references means evictable; combined with a
  VRAM budget this extends `AssetService`'s existing mesh residency to
  textures (R8) and makes *"graphics tuned down"* a number rather than a hope.

*Unlocks:* R1, R8, and all tooling.

### Phase 2 — Tools (fall out of Phase 1 almost for free)

1. **VRAM census** — every resident resource with size, refcount, owner;
   sorted by cost. Extends `RenderStatsChannel`.
2. **Duplicate report** — same content hash under two handles. After Phase 1
   this should always be empty, which makes it a *regression test*, not a
   diagnostic.
3. **Leak detector** — load scene → unload → assert resident VRAM returns to
   baseline. Run it N times in the soak lane; a monotonic climb is a leak.
   This is the mechanically checkable definition of "no render memory leaks".
4. **Per-material profile** — bytes attributable to each material, so an
   artist can see that one pistol costs 40 MB.

*Unlocks:* the `hardened` tier for `src/render`, which today has no test at all.

### Phase 3 — RenderWorld: finish the extraction split (R2) — **DONE**

Culling, sorting and light packing now live in `src/render/world/` (`rworld::`)
instead of inside `ForwardPipeline`, producing a **`RenderWorld`** consumed
through `RenderView::world()` / `::camera()`, with **packed 64-bit sort keys**.

A custom pipeline inherits culling, sorting and light data for free and only
decides *how* to draw — which is what "customizable" has to mean to be real.

Landed, with the deviations worth recording:

- **`RenderItem` carries its own bounds**, copied at extraction. This is what
  removes the `Mesh` dependency and makes the whole directory GPU-free — and so
  testable. `tests/render_world_test.cpp` is the first test in `src/render`'s
  history that can fail on a culling or sort-order regression.
- **Buckets are not materialized.** One `VisibleSet` sorted by a key whose top
  bits are the blend class gives the same ordering with one array and one sort;
  separate opaque/transparent/shadow vectors would be three allocations to
  express what two bits already say.
- **`BlendClass` is always `Opaque`** until `RenderItem` carries one — the key
  handles transparency, nothing produces it yet. See `src/render/world/info.md`.

*Unlocked:* R2; R5/R7 are now cheap — `batchRunLength()` is the instancing hook
and is already tested.

### Phase 4 — Submission efficiency

- **Instancing** on the existing mesh-grouped sort (R5).
- **State dedup**: skip material binds identical to the previous draw (R7).
- **Bone palette per item, not per submesh** (R4) — the one immediate,
  isolated fix; it needs no architecture and can land first if wanted.
- Resolve the **71 MB render-target mystery** (R6) with a per-target
  breakdown; on a 128 MB budget a 57 MB unknown decides whether we ship.

### Phase 5 — Authoring: make it actually customizable (R3)

- **Shader assets**: a `ShaderCooker` feeding the existing cook pipeline, so
  shaders are cooked content with DDC caching like meshes and textures — not
  `#include`d arrays.
- **Material assets + variants**: materials become data with a shader
  reference and typed parameters, replacing the fixed
  `baseColor/normal/roughness/metallic` struct.
- Then a game can define its own look **without rebuilding the engine**, which
  is the stated goal and is impossible today.

### Phase 6 — Scale (deferred, with triggers)

LOD (trigger: draw counts or vertex load actually hurt), cascaded shadows
(trigger: shadow quality complaints at range), occlusion culling (trigger: a
scene where frustum culling is insufficient). Not before there is a scene that
demonstrates the need — measured, as with everything above.

### Budget: the forcing function

Every phase is checked against the target machine, because "runs on the potato
PC" is the only success criterion that cannot be argued with:

| | budget | today |
|---|---|---|
| total VRAM | **≤ 128 MB** (shared) | 147 MB ❌ |
| textures | ≤ 60 MB | 76 MB ❌ |
| render targets | ≤ 20 MB | 71 MB ❌ |
| draw calls | ≤ 500 | 15 ✅ |
| system RAM | ≤ 1.5 GB | unmeasured |

These numbers are the acceptance test for Phases 1–4, and they are why the
work starts with memory rather than with pixels.
