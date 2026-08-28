---
status: as-built
verified: 2026-08-28
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

> **Re-verified 2026-08-28 — this subsection was stale in the GOOD direction, and
> its headline finding no longer holds.**
>
> It read: "`src/render/passes/` (shadow, sky, opaque, transparency, post,
> resolve) is **scaffold — commented stubs, not in the build.** The single real
> pipeline is `ForwardPipeline`, 442 lines that do everything after extraction…
> The design was right; it stalled."
>
> **It un-stalled.** `src/render/passes/` no longer exists; it is
> `src/render/pipeline/` with real, built `opaque_pass.cpp` and `shadow_pass.cpp`.
> `ForwardPipeline` is **253 lines**, not 442, because the passes came out of it.
> `src/render/world/` now holds the GPU-free decision layer the June TODOs asked
> for — `RenderWorld`, cull streams, sort keys, light packing — with headless
> tests. Extraction → visibility → submission are separate stages today.
>
> Two things from the original finding DO survive, and they are the ones that
> matter for `rhi-design.md`:
>
> * **There is still no render graph.** Passes are split into files and called in
>   a fixed order; they do not declare reads and writes, so nothing derives
>   barriers or aliases transient targets. Splitting the code was necessary and is
>   not the same as the graph.
> * **The seam still leaks.** `include/engine/render.h` — named below as "a real
>   public extension point" — is a 12-line umbrella over `render_pipeline.h`,
>   `render_view.h` and `render_context.h`, and the last two hand out
>   `bgfx::TextureHandle`, `bgfx::FrameBufferHandle` and `bgfx::ViewId`. A second
>   pipeline is expressible; a second *backend* is not.

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
| **R1** ✅ (shipped path) | **GPU resources have no identity, no refcount, no dedup.** `TextureRegistry::addTexture` is a slot allocator: every call makes a new GPU texture. Two materials referencing the same image file get two copies of it in VRAM. Nothing tracks who references a resource, so nothing can know when it is free. | `src/render/texture_registry.h` — `addTexture` appends; `removeTexture` is manual and unreferenced by any owner | **Critical** |
| **R2** ✅ | **Extraction/submission split never happened.** Culling, sorting, light packing and material binding all lived inside `ForwardPipeline`. A dev who swapped the pipeline inherited *none* of it and had to rewrite culling and sorting to draw a single triangle differently. **FIXED in Phase 3** — all four moved to `src/render/world/`; the pipeline now owns only shaders, uniforms and binds. Material *assets* remain fixed-struct (R3). | `src/render/world/`, `forward_pipeline.h` | **Critical** |
| **R3** ✅ | **Shaders are compile-time blobs.** `.sc` sources are compiled to per-platform C arrays (`vs_triangle_mtl`, `_dxbc`, `_spv`) and `#include`d. There is no shader asset, no variant system, no runtime load. A game cannot add a material type without rebuilding the engine. | `forward_pipeline.h:34–104`, `shaders/*.sc` | **Critical** (for the customization goal) |
| **R4** ✅ | **Bone palette uploaded per submesh.** `setUniform(m_uBoneMatrices, …, boneCount*4)` sits inside `bindMaterial`, which runs once per submesh. The 73-bone zombie re-uploads its whole palette for every submesh, every frame. | `forward_pipeline.h` | **High as written, nil as measured** — **FIXED in Phase 4**: hoisted to once per item in both the main and shadow passes. Safe because bgfx uniform VALUES persist across submits (`BGFX_DISCARD_STATE` drops the pending update range, not the applied value) — the same property that lets the view-level light/camera uniforms be set once per view. But the severity was assessed by READING: `fps_shooter`'s only skinned mesh (Zombie, 73 bones) has exactly ONE submesh, so the redundancy in this scene was zero and the fix changed no measured number. It is correct for any multi-submesh skinned mesh, and GPU time is now instrumented so the cost would be visible if such content appears. |
| **R5** ✅ | **No instancing.** The sort already groups identical meshes adjacently — the setup for batching is done, the batch is not. | `vs_instanced.sc`, `ForwardPipeline` run loop | **DONE 2026-08-04.** The sort key already made adjacency mean batchability, so the run loop walks `rworld::batchRunLength` and collapses each run into one instanced submit. Measured: 20 001 objects go from 20 001 draws to **1**, and fps_shooter from 12 to 6. Restricted deliberately — skinned items carry a per-item bone palette in uniforms, submeshes need per-range index draws, and a data-driven material supplies its OWN program (instancing it would silently render with the wrong shader), so all three fall through to per-draw. |
| **R6** ✅ | **Render-target memory is ~5× the naive figure.** 1280×720 colour+depth for the scene and game framebuffers should be ≈14 MB; bgfx reports **71 MB**. Unexplained — candidates are Retina drawable scaling, D24S8 storage on Metal, and the 2-deep swap chain. | `RenderStatsChannel`, `[Renderer] Scene FB: 1280x720` | High |
| **R7** ✅ | **Redundant material binds.** `bindMaterial` re-set every uniform and texture per submesh with no comparison against current state. **IMPLEMENTED AND MEASURED AT ZERO** (issues.md R17): binds were already 1:1 with draws, so the dedup saved nothing. Kept anyway — it is what makes submesh expansion (R18) safe. | `pipeline/opaque_pass.cpp` | Medium |
| **R8** ⚠️ dev path only | **Textures are outside the residency system.** `AssetService` gained a mesh residency budget with LRU eviction; textures — the 76 MB — have none. | `asset_service.cpp` residency covers meshes only | **Reassessed 2026-08-04: not a shipping problem.** The shipped path is 40.7 MB against a 60 MB budget, and its textures DO go through `GpuResourceCache` (identity + refcount), so a budget could be applied when one is needed. The 76/100 MB figures were the DEV path, where the importers call `addTexture` directly and nothing dedups or evicts. Worth fixing for editor sessions on large projects; NOT worth fixing to hit a budget the shipped game already passes with 19 MB spare. |
| **R9** ◐ | No LOD, no cascaded shadows (one map, one caster), no occlusion culling, no texture streaming. **LOD BUILT AND FED** (issues.md R20/R21) — screen-height selection plus cooker-side vertex-clustering decimation, so levels are genuinely cheaper (62.2% of triangles on the kit) and carry their parent's material groups. Cascades / occlusion / streaming remain open. | `world/lod.h` | Medium (deferred) |

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
- ~~**Bone palette per item, not per submesh** (R4)~~ — **DONE**, and worth
  recording what it taught: the fix is correct but measured nothing, because the
  scene's only skinned mesh has one submesh. Read-derived severity ratings in
  this table should be re-checked against real content before they justify work.
- **GPU time is now instrumented** (`FrameGpuStats`, from `bgfx::getStats()`):
  fps_shooter over 600 frames is **GPU avg 2.66 ms, max 5.70 ms** against a CPU
  frame of 11.16 ms, at 13 draws with zero handle churn. Instancing and state
  dedup below should be justified against these numbers rather than assumed —
  13 draws is not a submission problem.
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

### Two asset paths, and only one of them ships

`engine_host` and `engine_player` do not load the same way, and conflating them
produced every wrong number in this document:

| | scene references | texture ingestion | dedup / refcount / census |
|---|---|---|---|
| `engine_host` (dev) | SOURCE `.fbx`/`.gltf` | `AsyncLoader` + Assimp/cgltf -> `TextureRegistry::addTexture` | **none** |
| `engine_player` (ships) | COOKED `.ctex` | `AssetService` -> `GpuResourceCache` | yes |

Found by wiring the Phase 2 census (which existed and was never called — the cache
it reports on was private with no accessor) and seeing it print `0 resources,
0.0 MB` while bgfx reported 100 MB of textures under `engine_host`.

Consequence for every future measurement: **VRAM and residency claims must come
from `engine_player --gpu-stats` against a built `dist/`.** `engine_host` is the
right tool for frame pacing and CPU work, and the wrong one for memory.

### Shadow pass instanced, and a maintainability audit (2026-08-04)

`vs_shadow_instanced.sc` plus the run loop. The hand-rolled cull from the previous
change was replaced by the SAME `rworld::buildVisibleSet` the colour pass uses, run
against a light-space `ViewCamera` — culling and *sorting*, and the sort is what
makes batching possible at all. A cull loop can cull; only a sorted set can batch.

2 000 cubes with `--shadows`, cumulative across the last three changes:

| | originally | after cull | after instancing |
|---|---|---|---|
| main pass draws | 2 001 | 1 | 1 |
| shadow pass draws | 2 001 | 244 | **1** |
| total bgfx draws | ~3 990 | 246 | **4** |

fps_shooter unchanged throughout (10 items, 1 culled, 6 draws). 20 k objects still
exits 0.

**Audit: can `src/render` be reasoned about in isolation?** Written up in
`src/render/issues.md`. Verdict: the decision layer yes, the submission layer no.
`world/` is genuinely bgfx-free (it mentions bgfx only in comments explaining its
absence), `diag/` is payload-agnostic, and nothing outside `src/render` includes
renderer internals. But `forward_pipeline.h` is **812 lines in a header**, of which
`render()` is 274 and `renderShadow()` 147 — and every change this month landed in
those two functions.

The audit also found **`src/render/passes/`: nine headers describing a second,
never-compiled renderer architecture**, referenced by nothing and in no CMake
target, with names (`opaque_pass`, `shadow_pass`) that collide with the
decomposition the real pipeline now needs. That is the concrete answer to the
isolation question: not while the tree holds two renderer designs and only one runs.
Recommendation is to delete it — `render()`'s real structure is now known from
measurement, and a pass list drawn up before any of it was measured is unlikely to
be the right shape.

### Shadow-pass cull — and primitives were never culled at all (2026-08-04)

The shadow pass walked every item. Fixed by culling against the **light's**
frustum, which had to be its own planes: an object behind the camera can still
cast a shadow into view, so reusing the camera's planes here would delete real
shadows. `rworld::extractFrustumPlanes` was lifted out of `Renderer::buildView`
so both frusta come from one implementation.

**The bigger find, hit while verifying it:** `PrimitiveLibrary::upload` never set
`Mesh::boundsMin/Max`. They stayed at ±infinity, `hasBounds()` returned false, and
the frustum test treats that as "never cull" — so **every primitive mesh was exempt
from culling, in the main pass as well as the shadow pass**, for as long as
primitives have existed. A 2 000-cube scene reported `0 culled` against a light box
44 units wide, which is what exposed it. Bounds now come from the vertices, which
`upload()` already had in hand.

Measured, 2 000 cubes with `--shadows`:

| | before | after |
|---|---|---|
| main pass | 2 001 items, **0 culled**, 2 001 draws | 2 001 items, **283 culled**, **1 draw** (1 718 instanced) |
| shadow pass | 2 001 draws from 2 001 items, no cull | **244 draws**, 1 757 culled by the light frustum |
| total bgfx draws | ~3 990 | **246** |

fps_shooter is unchanged — 10 items, 1 culled, 6 draws — so real content saw no
regression from either change.

**A note on reading cull numbers.** The first run after adding bounds reported 2 000
of 2 001 culled, which looks exactly like an over-culling bug — the dangerous
direction, where geometry silently vanishes. It was not: the stress generator's
camera used a pitched quaternion and ended up facing away from the grid. Verified
by pointing a known-good camera at the scene (283 culled, 1 718 visible) and by
fps_shooter being untouched. The generator now uses an identity rotation for exactly
this reason — a stress scene whose camera orientation you have to reason about is
one that will mislead you.

Still not instanced: the shadow pass. Its 244 draws share mesh and material and
could collapse the same way the main pass did, which needs a `vs_shadow_instanced`
variant. Worth doing, and smaller than what it follows.

### Instancing landed, and it moved the bottleneck (2026-08-04)

`vs_instanced.sc` reads the model matrix from instance data; the submit loop walks
runs instead of draws and collapses each into one submit.

| scene | draws before | draws after | GPU before | GPU after | CPU frame before | after |
|---|---|---|---|---|---|---|
| 2 001 objects | 2 001 | **1** | 0.82 ms | 0.44 ms | 9.33 ms | 9.76 ms |
| 20 001 objects | 4 096 + **15 905 dropped** | **1** | — | 0.96 ms | 55.82 ms | 38.76 ms |
| fps_shooter | 12 | 6 | 2.55 ms | 2.53 ms | ~13 ms | ~16 ms |

**The 8 MB uniform-buffer wall is gone as a practical concern.** At 20 000 objects
the draw ceiling is never reached — one instanced submit costs one draw's worth of
uniform bytes instead of 20 000, so instancing bought the crash headroom rather
than a bigger buffer. The guard stays as a backstop.

**And the bottleneck moved, which is the important part.** 20 001 objects now
submit ONE draw call and still cost 38.76 ms of CPU per frame. That cost is no
longer submission — it is extraction and culling and sorting 20 001 items: a
~152-byte `RenderItem` built per item per frame (≈3 MB touched), a sorted array
rebuilt every frame, and `VisibleDraw` holding only `{key, index}` so the submit
side random-accesses that AoS.

**So the flat-render-packet redesign now has its evidence.** It was fourth in line
because there was no measurement to justify it; there is one now, and it is the
next item rather than the last. What it should target is the per-ITEM path, not the
per-draw path — extraction and the sort, where the remaining 38 ms lives.

Revised order:
1. ~~The crash~~ — root-caused and guarded.
2. ~~Instancing (R5)~~ — done.
3. **The shadow pass cull.** Still submits every item; with instancing in the main
   pass it is now the larger of the two passes by far.
4. **Extraction / flat packets.** The 38 ms at 20 k with one draw call. Cull already
   copies bounds to avoid pointer-chasing; the remaining costs are building 152-byte
   items and the per-frame sort.
5. **R7 material-bind dedup.** 6 binds for 6 draws in fps_shooter — worth less now
   that instancing removed the bulk of the binds, but still free headroom.

### The stress scene, and a hard wall at ~10 000 objects (2026-08-04)

`scripts/gen_stress_scene.py <dir> --objects N [--shadows]` generates a project of
N primitive cubes — no assets, no cook, no DDC, so it loads instantly at any N and
is deterministic. Every object shares one mesh and one material, which makes
`draws` vs `batchRuns` report the instancing CEILING rather than a sample of it.

    ./build/engine_host <dir> --frames 320

| objects | main draws | batch runs | GPU ms | CPU frame ms | outcome |
|---|---|---|---|---|---|
| 12 (fps_shooter) | 12 | 3 | 2.55 | ~11.5 | 120 fps |
| 2 001 | 2 001 | 1 | 0.82 | 9.33 | fine |
| 8 001 | 8 001 | 1 | 1.44 | **33.24** | ~30 fps, CPU-bound |
| 2 001 + shadows | 2 001 + **2 001 shadow** | 1 | 0.99 | 9.83 | shadow doubles draws |
| ≥ 10 000 | — | — | — | — | **SIGSEGV / SIGBUS** |

**Submission, not the GPU, is the wall.** GPU time stays between 0.8 and 2.6 ms
across the whole range — 2 000 untextured cubes are CHEAPER on the GPU than
fps_shooter's 12 textured meshes. CPU frame time goes 9.3 ms at 2 k to 33.2 ms at
8 k, i.e. roughly linear at ~4 µs per draw. That is the number that justifies
instancing and the flat-packet redesign, and it did not exist before this scene.

**The shadow pass has no cull, confirmed.** With `--shadows` at 2 000 objects it
submits 2 001 shadow draws for 2 001 items — every item, regardless of the light
frustum — roughly doubling total bgfx draws. `shadowDraws == itemsConsidered` is
the signature.

**ROOT-CAUSED (2026-08-04): bgfx's Metal uniform scratch buffer, 8 MB, no bounds
check.** ASan named it in one run:

    #2 bgfx::mtl::RendererContextMtl::commit(bgfx::UniformBuffer&) renderer_mtl.cpp:1986
    #3 bgfx::mtl::RendererContextMtl::submit(...)                  renderer_mtl.cpp:5335
    #7 EngineRuntime::frameEnd()                                   runtime_frame.cpp:80
    The signal is caused by a WRITE memory access.

`renderer_mtl.cpp:23` is `#define UNIFORM_BUFFER_SIZE (8*1024*1024)`. Per-draw
uniform data is written into that fixed allocation at an advancing offset with NO
bounds check, so once a frame's uniform traffic exceeds 8 MB bgfx writes past the
end of the Metal buffer. This pipeline costs a MEASURED ~1035 B/draw (8 MB ÷ the
~8100-draw empirical threshold), giving a hard ceiling near 8 192 draws — which is
why 8 000 objects renders 320 frames clean and 8 100 does not, and why the boundary
is flaky rather than sharp: it is a byte limit, not a count.

**A claim in the previous revision of this section was wrong.** It said the crash
was "independent of draw count" because turning the camera away still crashed. ASan
disproves that — the overflow is in per-DRAW uniform commit, so it is entirely
draw-dependent, and that experiment simply failed to cull what it intended to.
Recorded because the inference was confident and wrong, and the sanitizer settled
it in one run where three rounds of reasoning had not.

**Guarded, not merely diagnosed.** `ForwardPipeline::kMaxDrawsPerFrame = 4096`
stops submitting and says so, once, loudly, with `SubmitStats::drawsDropped`
reporting how many draws were refused so an incomplete frame can never be silent.
4096 rather than "just under 8192" for two reasons: a cap near the limit still
overflows (7 800 was tried and still crashed), and this engine's own budget target
is **500** draw calls, so 4096 is already 8× the envelope and clamping there costs
nothing real. Verified: 20 000 objects now exits 0 where it previously took SIGSEGV,
8 000 is unaffected, and 2 000 never trips the warning.

Trading a crash for a visibly incomplete frame is a band-aid, and it is labelled as
one in the code. The real fix is fewer uniform bytes per draw — material-bind dedup
(R7) and instancing (R5) — not a bigger buffer, and certainly not a vendored bgfx
patch that would only move the wall.

Order this implies, evidence-first:
1. ~~**The crash.**~~ Root-caused and guarded.
2. **The shadow pass cull.** O(N) with scene size, doubles draws — and it now also
   consumes half the uniform budget, so it brings the ceiling twice as close.
3. **Material-bind dedup (R7).** 12 binds for 12 draws today. This is the item that
   directly buys back uniform-buffer headroom, which makes it worth more than its
   CPU saving alone suggests.
4. **Instancing (R5).** 4 µs/draw × (draws − batchRuns), now computable.
5. **The flat packet.** Worth it when the submit loop's cache misses are visible
   above what the first four leave behind.

Still open: the ASan build was rebuilt with the guard but the verification run was
not completed (an 8-minute incremental link). The non-sanitized evidence — SIGSEGV
to clean exit at both 8 k and 20 k — is what the fix rests on.

### The submission seam, and what it says about R5/R7 (2026-08-04)

Submission was the one part of the renderer with no numbers: bgfx's Noop backend
never sets `numDraw`, so draw counts could not be asserted headlessly, and R4/R5/R7
were argued from reading. `render/submit_stats.h` counts on OUR side — works on any
backend — and each counter states a finding directly:

    [Submit] engine_host
          items        10 considered, 1 culled
          draws        12  (5 from submeshes, 1 skinned)
          batch runs   3  -> instancing would remove 9 submit(s)   [R5]
          material     12 bind(s) for 12 draw(s)                   [R7]
          bones        1 upload(s) for 1 skinned item(s)           [R4]
          shadow       0 draw(s), 0 bone upload(s)

- **R4 is now machine-checked**, not just fixed: `bonePaletteUploads` must equal
  `skinnedItems`, and the report warns if it ever equals `skinnedDraws` instead.
- **R5 has a number for the first time**: 3 batch runs for 12 draws, so instancing
  would collapse 12 submits to 3. A 75% reduction — of twelve. Still not worth
  building without a scene where 12 is 12,000.
- **R7 is confirmed exactly as written**: 12 binds for 12 draws, no dedup at all.
- **The shadow pass submits 0 draws in this scene** — no caster — while still
  holding a 2048² D32F shadow map, which is 16 MB of the 23 MB render-target
  total. Worth knowing before trimming RT: most of it is reserved for a pass that
  does nothing here.

Also note what the seam does NOT fix: the shadow pass walks EVERY item rather than
a culled set (`for i < v.items.size()`), so `shadowDraws` will grow with scene size
while `draws` stays flat. Dormant at 10 items; an O(N) problem at scale, and one no
amount of packet compaction addresses.

### Decision not taken — the bgfx render thread (measured 2026-08-03)

`renderer.cpp` calls `bgfx::renderFrame()` before `bgfx::init` to force
single-threaded mode. The note there used to say a render thread "races with
platform data / a null window". **That does not reproduce**: `platformData.nwh`
goes through the `Init` struct before `bgfx::init`, which is the documented-safe
order, and removing the call runs 600 frames without a crash.

What it does instead is worse and harder to spot. Three runs each of
`engine_host fps_shooter --frames 600`:

| | mean cadence | fps | worst frame |
|---|---|---|---|
| single-threaded | 8.36 / 8.33 / 8.33 ms | 119.6 / 120.0 / 120.0 | 30.3 / 16.0 / 20.2 ms |
| render thread | 10.17 / 8.39 / 10.18 ms | 98.3 / 119.1 / 98.3 | **1008.0 / 41.0 / 1008.4 ms** |

Two of three runs stall for **~1 second**. Not bgfx's API semaphore giving up —
that timeout is 5000 ms. The round ~1 s, plus the "blocked waiting for next
drawable" stack from an earlier Instruments capture, points at Metal drawable
acquisition starving once submit and render are pipelined.

Even the clean run buys nothing. Per-frame CPU `work` is 0.38 ms against an
8.33 ms display period, so there is no submit cost to overlap with rendering —
`present` is 95% of the frame. Pipelining also adds a frame of latency by
construction (`bgfx::frame` waits on the render thread, which is rendering the
PREVIOUS frame), which is against the motion-to-photon budget this engine exists
to serve.

**Revisit when, and not before:** `FrameStatsChannel`'s `work` approaches the
frame period — when the main thread's own CPU work caps the frame rate instead of
the display. The instruments to watch are `waitSubmit`/`waitRender`, which read 0
in single-threaded mode and were 3.07 / 7.66 ms in the experiment: both threads
mostly waiting, because neither was the bottleneck.

### Phase 6 — Scale (deferred, with triggers)

LOD (trigger: draw counts or vertex load actually hurt), cascaded shadows
(trigger: shadow quality complaints at range), occlusion culling (trigger: a
scene where frustum culling is insufficient). Not before there is a scene that
demonstrates the need — measured, as with everything above.

### Budget: the forcing function

Every phase is checked against the target machine, because "runs on the potato
PC" is the only success criterion that cannot be argued with:

**MEASURE THE SHIPPED PATH. The original figures did not** (corrected
2026-08-04). `engine_host` is the dev runner: `fps_shooter/assets/scenes/main.scene`
references SOURCE assets (`.fbx`, `.gltf`), so it loads through `AsyncLoader` +
Assimp/cgltf, which call `TextureRegistry::addTexture` directly — no dedup, no
refcount, no census. The shipped game loads COOKED assets through `AssetService`,
which does go through `GpuResourceCache`. Those are different numbers for
different code, and the acceptance test only means anything for the one that ships:

    ./build/engine_build fps_shooter
    ./build/engine_player fps_shooter/dist --gpu-stats --budget <tier> --frames 300

| | budget | dev path (`engine_host`) | **SHIPPED (`engine_player`)** |
|---|---|---|---|
| total VRAM | **≤ 128 MB** (shared) | 123.1 MB | **63.7 MB ✅** |
| textures | ≤ 60 MB | 100.0 MB | **40.7 MB ✅** |
| render targets | ≤ 20 MB | 23.0 MB | **23.0 MB ⚠️** |
| draw calls | ≤ 500 | 13 ✅ | **12.9 ✅** |
| GPU time | (added) | 2.55 ms | **2.49 ms** |
| system RAM | ≤ 1.5 GB | unmeasured | unmeasured |

The audit's original 147 MB / 76 MB, and the 123 / 100 measured while adding GPU
timing, were all the DEV path. **The shipped game is at half its VRAM budget**, and
textures pass with 19 MB to spare. This is why the census got wired first: it read
`0 resources, 0.0 MB` under `engine_host` while bgfx reported 100 MB, which is what
exposed the two-path split.

The one remaining miss is render targets, 23.0 vs ≤20 MB, and it is not a mystery
any more (that was R6): **16 MB of it is the 2048² D32F shadow map**, and the scene
framebuffer is Retina-scaled — the window is 1280×720 but bgfx reports a
2560×1440 drawable, 4× the naive area. Both are settings, and shadow resolution is
already a project quality option (`08d28c9`).

Biggest single texture cost on the shipped path, if it ever needs to come down:
one asset carries two 4096² maps (10.7 MB + 21.3 MB = 32 MB of the 40.7). A
cook-time resolution cap would take that to 8 MB. Not needed today — recorded so
the lever is known rather than rediscovered.
