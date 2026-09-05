---
status: as-built
tier: working
verified: 2026-09-05
covers:
  - src/render/
tests:
  - tests/gpu_cache_test.cpp
  - tests/render_registry_test.cpp
  - tests/render_pipeline_test.cpp
# `working` as of 2026-08-04, and the thing that changed is the one this note has
# been naming as the blocker for weeks: SUBMISSION is tested. It was untestable
# because bgfx's Noop backend never sets numDraw (its submit() writes timing,
# zeroes numPrims, and touches nothing else), so draw counts cannot be read back
# from bgfx headlessly. render/submit_stats.h counts on OUR side of the API, which
# works on any backend — and render_pipeline_test builds a RenderView by hand (no
# ECS, no Renderer, no window) and asserts the submission decisions: cull wiring,
# batch runs, the instanced path AND its negative case, the uniform-buffer draw
# ceiling, R4's one-palette-per-skinned-item invariant, the shadow pass's separate
# light-space cull, and that the counters reset per view. Mutation-checked: dropping
# render()'s stats reset, and disabling the instanced path, each fail it.
#
# NOT `hardened`, and the gap is specific: no pixels. Noop executes nothing, so
# "the shadow map is sampled with the right matrix" and "vs_instanced.sc reads
# i_data0..3" are outside any headless test's reach — instancing, the shadow cull
# and shadow instancing are verified by COUNTS and timings, never visually. That
# needs a real-device harness (golden-image or GPU-timing based), and it is the
# next thing that would raise this tier. There is also no fuzz/soak/stress lane
# over the render path.
#
# INSTANCING (R5) is live: the submit loop walks rworld::batchRunLength and
# collapses runs sharing mesh+material+SUBMESH into one instanced submit
# (vs_instanced.sc, model matrix from instance data). 20 001 cubes -> 1 draw call, and
# since R18 submeshed meshes instance too: 50 000 real props went from 3 067 draws with
# 534 REFUSED to 299 draws. Skinned items and data-driven materials still fall through
# to per-draw on purpose — a skinned item's bone palette is per-ITEM (R4) and a
# data-driven material supplies its own program.
#
# MEASURED on the stress scene (scripts/gen_stress_scene.py), which is reproducible
# in a way a hand-built project is not. At 20 000 objects the Render.* zones read
# extract 1.13 ms, cull 0.52, shadow 0.28, submit 0.13 — the whole render path is
# ~2.1 ms and the frame is vsync-bound. Extraction went 15.2 -> 1.1 ms over five
# changes (issues.md R14/R15); it is now parallel over archetype chunks on
# engine::jobs, and so is the frustum cull (R16: 3.9 ms -> 0.83, with std::sort
# replaced by a stable radix sort, 1.15 -> 0.22). The cull now reads SoA streams
# (world/issues.md A2.P1) rather than the 144-byte RenderItem, which is what took the
# SHADOW pass from 0.48 ms to 0.15 at 100 k — its cull was rebuilding every bounding
# sphere the camera pass had just built. At 100 000 objects: extract 5.41, cull 1.37,
# shadow 0.15, submit 0.36, render 7.29, frame ~9.0 ms in ONE draw call. Extraction is
# the largest phase and is already parallel. NEON was measured and DECLINED (the plane
# arithmetic is ~4% of the cull, so 2.3x on it is 0.4% of the frame), and RenderItem is
# down to 128 bytes with two write-only fields deleted (A3). Both were small, and A3
# records why: extraction is parallel over 12 cores, so per-item savings divide by the
# core count. The next lever is incremental extraction, not micro-optimisation.
# R7 (material-bind dedup) measured ZERO benefit when it landed (issues.md R17) and now
# works, because R18 removed the reason it could not: submesh ranges are expanded BEFORE
# the sort and carry their own material, so identical materials are adjacent and the
# one-deep cache (the only CORRECT depth — bgfx holds one uniform set at a time) hits.
# 5 000 real props: 2 869 binds for 2 869 draws -> 299 for 299.
#
# Diagnostics go through core/logger.h with the `Renderer` tag, never printf, because
# Logger also feeds the EDITOR CONSOLE — see the Diagnostics section below.
---
# Render

## Purpose
Everything between the ECS and the GPU: render data extraction, the swappable
pipeline, pass scheduling, GPU resource registries, and vertex formats.
Backend is bgfx (Metal on macOS, D3D/Vulkan elsewhere).

## Architecture
Two layers:

1. **`Renderer`** (`renderer.h`) — owns the device and produces the
   `RenderView`. Four concerns, one TU each, because it was one 437-line file
   mixing bgfx init with the hot loop:
   - `renderer.cpp` — pipeline ownership: attach/detach and `makeContext()`.
     Both `setShadowResolution` and `setShaderCacheRoot` live here for one
     reason: the pipeline builds programs and targets in `onAttach`, ONCE, so a
     change after init must re-attach or it silently keeps the old programs.
   - `renderer/device.cpp` — `init`/`shutdown`/`resize`/`frame`, and the bgfx
     allocator that tags every bgfx byte to the Rendering heap. Records the
     measurements behind single-threaded bgfx.
   - `renderer/targets.cpp` — the scene/game framebuffers and the three
     `render*` entry points, which differ only in which target they pick.
   - `renderer/extract.cpp` — queries `Transform + MeshRenderer` and
     `Transform + Light` into flat `RenderItem`/`LightItem` arrays. The pipeline
     never touches the ECS. Was the hot path — 15.2 ms at 20 000 objects, now
     1.1 ms — via partitioned/optional query terms, a direct SRT compose, and
     `jobs::parallelFor` over archetype chunks. The file header carries the full
     measurement history and the reason SIMD was not the answer.
2. **Pipeline** — `IRenderPipeline` (`render_pipeline.h`) is the swap point
   (`Renderer::setPipeline`). There is exactly ONE implementation,
   `ForwardPipeline`, split one concern per TU:
   - `forward_pipeline.h` — the class declaration, and nothing else.
   - `pipeline/programs.cpp` — programs, uniforms, shadow map (onAttach/onDetach).
     The only includer of `pipeline/shader_blobs.h`, whose arrays are `static`.
   - `pipeline/opaque_pass.cpp` — `render()`: visible set → material binds →
     instanced runs → submits, plus the debug-line pass.
   - `pipeline/shadow_pass.cpp` — `renderShadow()`: light matrices, light-space
     visible set, instanced casters.

   A `passes/` directory used to sit here describing a second, pass-list
   architecture — nine headers, in no CMake target, included by nothing, never
   compiled. It was deleted (see `issues.md` R11): two renderer designs in one tree
   with only one live is the thing that stops the subsystem being readable, and the
   real structure of `render()` is now known from measurement rather than guessed at
   before any existed.

## Key Data
- **Registries** — `AssetRegistry` (meshes), `TextureRegistry`,
  `MaterialRegistry`: `Handle<Tag>` dense vectors, slot 0 = null handle.
- **Vertex formats** — `vertex.h` (**48** bytes: pos 12 + normal 12 + tangent 16
  + uv 8 — this said 52 until 2026-09-04, when `gpu.cpp`'s `static_assert`
  settled it), `skinned_vertex.h` (68 bytes: the same plus 4×uint8 joints +
  4×float weights). Neither declares its own GPU layout any more; see below.
- **`Mesh`** (`mesh.h`) — VB/IB handles + `SubRange` submeshes (skinned
  imports merge all body parts into one VB/IB with submesh ranges). The handles
  are `gpu::` types, not backend ones.

## `IRenderer` and `NullRenderer` — G1c step A

`renderer_interface.h` is what `EngineRuntime` drives; `Renderer` and
`NullRenderer` (`renderer_null.h`) are its two implementations, and
`runtime_boot.cpp` is the only runtime TU that names either.

**It is not a performance seam.** All 26 methods through a virtual is ~18 ns a
tick — 0.00005% of a 30 Hz server tick, and most of them are boot-time (`docs/rhi/headless.md` §1). It exists
because the alternative was `if (!m_headless)` scattered across the runtime, and
that design shipped a defect: `frame()` was guarded and
`engineDrawSubmitBindRenderer()` was not, so a dedicated server's submission list
filled at ~480 KB/s forever (`src/runtime/docs/issues.md`, 2026-08-10). A null
object has no guard to forget.

Three properties worth keeping:

* **Every method is pure virtual**, so adding one is a compile error until
  `NullRenderer` implements it. That is why axiom 5's "the second implementation
  rots" hazard does not apply — it warns about differing ANSWERS, and a
  do-nothing implementation has none. Mutation-checked.
* **Three queries are exceptions** and their null values are decisions, not
  defaults: `homogeneousDepth()` is false, `sceneW()`/`sceneH()` are 0. A
  plausible fake size would invite an aspect ratio computed off a surface that
  does not exist. Pinned by `tests/null_renderer_test.cpp`.
* **`frame()` and `endFrame()` are both on the interface and both unconditional.**
  The runtime no longer chooses between them, which is the specific mistake that
  leaked.

**What it does NOT do: make a lean server.** `engine_runtime` still links bgfx —
the binary contains both implementations. Excluding the render TUs is a build
target (`docs/rhi/phases.md` G1c step B) and is not started.

## The upload seam (`gpu.h`) — G1a

`render/gpu.h` is the ONLY thing outside `src/render/renderer/` that turns
engine bytes into GPU handles, and `gpu.cpp` is the only TU that implements it.
Opaque `VertexBufferHandle` / `IndexBufferHandle` / `TextureHandle`, an opaque
`Blob` for staging, and five operations: stage, create vertex/index/texture,
destroy.

**The rule it enforces:** *loading produces bytes, uploading produces handles,
and only the second needs a GPU.* Five files used to do both in one function —
`asset_service.cpp`, `async_loader/upload.cpp`, `mesh_loader.cpp`,
`gltf_importer.cpp`, `assimp_importer.cpp` — which is what blocked a headless
server, a second backend and an embedding host simultaneously.

Three things worth knowing:

* **The two vertex layouts live in `gpu.cpp`**, not on the structs. `Vertex` and
  `SkinnedVertex` carry a `kGpuFormat` enum instead. `layout()` returned a
  `bgfx::VertexLayout`, which put the backend into every header that named a
  vertex — and from there into two asset importers. The struct and its
  descriptor are now in different files, so `gpu.cpp` `static_assert`s both
  strides: the half of the agreement a compiler can check.
* **Texture formats are checked BEFORE staging.** `gpu::textureFormatSupported()`
  is a thread-safe capability query (caps are fixed at device creation), and
  every loader asks it on the worker before spending the memcpy. This is not
  hygiene: refusing inside `createTexture2D` strands the staged payload for the
  life of the process — the backend frees staging memory only when a command
  consumes it — and a build cooked for the wrong target takes that path for
  *every* texture in the scene. `createTexture2D` re-checks as a backstop and
  prints `BUG:` with the stranded byte count rather than failing quietly.
  Reported once per format, not once per texture. `tests/gpu_seam_test.cpp`.
* **`gpu::deviceAvailable()` gates everything.** Set by `Renderer::init`,
  cleared by `Renderer::shutdown` *before* bgfx goes down. With no device,
  staging returns null and every create returns invalid — so the asset path
  runs to completion and simply produces no handles. Tests that want a device
  use `tests/gpu_test_device.h`, never `bgfx::init` directly.
* **`gpu_bgfx.h` is the deliberate escape hatch** for `src/render/` only: the
  passes still call `bgfx::submit`, so they convert back with `gpu::toBgfx()`.
  It is deleted, not ported, when the RHI lands.

`scripts/check_gpu_seam.py` runs in the `unit` lane and fails the build if any
file outside `src/render/` includes a graphics API. That is the part that keeps
this true, because the coupling returns as one `#include` added to fix one
compile error, not as a decision.

**G1b/G1c, 2026-09-04.** Every renderer header is now backend-free:
`renderer.h`, `render_view.h`, `render_context.h`, `mesh.h`, `texture.h`,
`vertex.h`, `skinned_vertex.h`, `asset_registry.h`, `primitive_library.h`. They
use `gpu::TextureHandle` / `FrameBufferHandle` / `ViewId` / `ClearFlags`, and
the `.cpp` files convert with `gpu::toBgfx()` at the point of the actual driver
call.

Two consequences worth stating:

* **A third-party `IRenderPipeline` can be written without a graphics API in
  scope.** "Swap the whole renderer by assigning a different IRenderPipeline"
  was not previously a claim anyone outside this repo could act on, because
  `RenderContext` handed them `bgfx::TextureHandle` and `bgfx::ViewId`.
* **`gpu::ViewId` is a plain `uint16_t` alias, not an opaque handle.** View ids
  are compared, incremented and indexed throughout the passes; wrapping them
  would be ceremony with no defect behind it. Aliasing prejudges nothing — the
  render graph deletes the concept either way.

`tests/headless_include_probe.cpp` compiles `runtime.h` and the pipeline seam
with bgfx absent from the include path. That catches what
`check_gpu_seam.py` cannot: the coupling here was TRANSITIVE, and a header
inside `src/render/` re-adding an include is legal to the grep and fatal to the
probe.

**Still open:** `engine_runtime` LINKS bgfx — the runtime calls ~15 `Renderer`
methods unconditionally, so the symbols are needed even where no device is
created. See `docs/rhi/phases.md` G1c for the null-renderer decision.

## Skinning (GPU)
Bone palettes are uploaded as a vec4 array uniform (`u_boneMatrices`,
128 bones × 4 vec4), **ONCE PER ITEM — not per submit** (audit R4). bgfx uniform
VALUES persist across submits, so one upload covers every range that item draws;
uploading inside the material bind re-sent 4.7 KB per range per frame for identical
data. This is also why skinned items are the one thing R18 does NOT expand into
per-range draws: expansion scatters an item's ranges across the sorted list, other
items' palettes land in between, and each range would need its own re-upload. The
vertex shader (`vs_skinned.sc`) reconstructs mat4s and does 4-influence linear blend
skinning.

## View IDs
0 = shadow, 1 = scene (offscreen FB), 2 = backbuffer clear, 3 = MSAA resolve,
4 = game view; 5+ allocated via `m_viewCursor`. ImGui uses high ids (editor).

## Shadow pass
Depth-only, one 2048² map, first shadow-casting light. It binds **no material** — the
program is fixed — which is why a caster whose submesh ranges tile its index buffer is
drawn as ONE whole-buffer draw rather than range by range (`Mesh::submeshesTile()`,
issues.md R19): same triangles, one draw, and submeshed casters can instance. A mesh
that fails the tiling check falls back to per-range draws and logs once.

The flip side of ignoring materials: **alpha-tested casters cast solid shadows.** Foliage
and fences need an alpha-test shadow variant selected per range, which is a feature, not
a fix, and would partly undo the range collapse where it applies.

## LOD
Discrete levels, selected at extraction (`issues.md` R20). **Level 0 is
`MeshRenderer::mesh`**; `LodMesh` (`components/lod_mesh.h`) holds up to three coarser
levels and their thresholds, so an entity with no chain renders at full detail and
deleting the component is a valid way to say so.

The metric is **screen height, not distance**: `h = r * projYScale / d`, with
`projYScale = proj[5] = 1/tan(fovY/2)`. Distance thresholds break on a changed FOV
(zooming a scope would coarsen the world) and on mixed object scale; a ratio of the
viewport is also resolution independent. Selection itself is a pure function in
`world/lod.h` and is the only part with a test that can fail.

Four properties that are load-bearing rather than incidental:

- **Selection repairs the sort key.** It runs after `writeCullEntry`, so `keyBase`
  already has level 0's mesh/material ids packed in. Leaving it stale would let the
  submit loop instance items at different levels together and draw them all with one
  mesh. Mutation-verified: removing the repair moved 299 draws → 395.
- **Bounds stay level 0's**, so an object's cull result cannot flip as it crosses a
  threshold, and the screen height that chose a level is not altered by the choice.
- **A broken chain degrades to MORE detail.** An unresolved level falls back toward
  level 0 and is counted (`Renderer::lodCensus().broken`, plus one latched `LOG_WARN`).
- **A level keeps its parent's MATERIAL GROUPS** (cooked v5). Levels used to be one
  range drawn with `material[0]`, so a prop with two groups changed colour the moment
  it crossed a threshold — 96 of the MegaKit's 176 meshes have more than one.
  Decimation rebuilds the index buffer group by group, and the output ranges tile
  from zero, which is what keeps `submeshesTile()` true and the shadow pass on its
  one-draw path.

`lodCensus()` reports how many items landed on each level for the last view extracted,
plus the triangles those levels cost against what level 0 everywhere would have cost.
LOD's whole job is to be invisible, so per-level counts are the only way to tell a
working chain from an inert one. `engine_host` prints it every 300 frames, and stays
silent when the scene has no chains.

**The census counts what is DRAWN, including the items that opt out.** An item whose
sphere carries a sentinel radius (no bounds, or unbounded) keeps full detail, and it is
recorded as a level-0 decision with its triangles in BOTH totals. It used to bank them
into the counterfactual only, which reported a saving that had not happened — the one
thing a counter justifying a feature must never do.

**LEVELS COME FROM THE COOKER, and only the cooker.** They resolve synchronously from
cooked meshes: `loadMesh`'s last resort is an Assimp import on a worker that completes
by setting a `MeshRenderer`, which cannot fill slot 2 of an `LodMesh`. An unresolved
level shortens the chain.

**Not built, and known:** an inspector for authoring chains, async level loads,
cross-fading (switching is a hard pop), and max draw distance. Skinned meshes get no
chain — R18 does not expand skinned items, so a level would never be selected.

## Diagnostics

Renderer events go through **`core/logger.h` with the `Renderer` tag**, not `printf`.
That is not cosmetic: `Logger` writes to stdout *and* keeps a ring buffer the **editor
console** reads, so a `printf` diagnostic is invisible to anyone not watching a
terminal. The draw-ceiling message — the one that says the frame on screen is missing
geometry — was on `stderr` until 2026-08-05.

Levels mean something here:

| level | when | example |
|---|---|---|
| `LOG_ERROR` | output is wrong or the device is gone | draw ceiling hit (**frame incomplete**), null window handle, bgfx init failed |
| `LOG_WARN` | silently degraded, still rendering | no `BGFX_CAPS_INSTANCING`, instance buffer exhausted, material's shader missing from the cooked cache |
| `LOG_INFO`/`SUCCESS` | one-time facts worth seeing | scene framebuffer size, shadow-map size and cost, which standard program was chosen |

**EVERY renderer log site is latched or deduped**, and that is a hard rule rather than
a style preference: these live in or beside the submit loop, and a diagnostic that fired
per draw would cost more than the thing it reports and bury the log it belongs in. The
ceiling uses `m_warnedDrawCeiling`, missing shaders a `std::set` of names, data-driven
binds a set of material+shader pairs, and the two instancing degradations their own
`m_warned*` bools. Add a log here only with a latch.

`render/diag/diag_report.h` deliberately keeps `printf`: it renders multi-line REPORTS
(submit counters, VRAM census) on demand from tools, not events, and pushing eleven
lines per report into the editor console would be noise.

## Invariants
- Row-major matrices, row-vector convention (`v * M`), matching bx.
- Scene renders into an offscreen FB at ≥ window resolution (panel downscale
  = free SSAA); recreate only on >8px delta (resize-thrash hysteresis).
- All GPU resource creation/destruction on the main thread.

## Future Work
- Pass injection API (add a custom pass without replacing the pipeline).
- Material system beyond base-color/normal (PBR params, shader variants).
