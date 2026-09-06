---
status: as-built
verified: 2026-09-06
covers:
  - src/render/
---
# Where this renderer stands against a production one

Written because the question "did we make an architectural mistake, is bgfx the
bottleneck?" is worth answering once with numbers instead of re-deriving it. Every
figure here was measured this week on an M-series Mac; the stress scenes are
`scripts/gen_stress_scene.py` (synthetic) and `scripts/gen_fuzz_scene.py` (real cooked
assets from a 176-model kit).

## Short answer

**bgfx is not the bottleneck, and the architecture is not a mistake.** What limits the
renderer today is its **draw-submission model** — one `submit()` per draw, each carrying
its own uniform payload — and one genuine design debt of our own: submesh draws are not
first-class in the visible set. Both are fixable without leaving bgfx.

## The limit that gets blamed on bgfx

The 4 096 draw ceiling is **ours**, not bgfx's. bgfx's own limit is
`BGFX_CONFIG_MAX_DRAW_CALLS` = 65 535. Our `kMaxDrawsPerFrame` guards a different
thing: bgfx's Metal backend commits per-draw uniforms into a **fixed 8 MB scratch
buffer with no bounds check** (`UNIFORM_BUFFER_SIZE`, renderer_mtl.cpp), so around
8 192 draws it writes past the end and segfaults inside `commit`. The missing bounds
check is a real bgfx weakness. Hitting it at all is ours: we spend roughly 1 KB of
uniform payload per draw.

Likewise the 4 096 vertex/index-buffer wall that once dropped 92% of a scene is
`BGFX_CONFIG_MAX_*_BUFFERS`, overridable with `-D`, and the actual bug was that we
created a buffer pair per `loadMesh` CALL instead of per cooked mesh.

**bgfx already supports what a production submission model needs** — compute shaders,
indirect draws, multi-threaded encoders (`bgfx::begin()`), instancing. We use one of
those four. The headroom is unused, not absent.

## Feature-by-feature

| dimension | this renderer | production-grade |
|---|---|---|
| submission | one `submit()` per draw; per-draw uniforms | **indirect draws** from GPU-built argument buffers; per-draw data in one structured buffer indexed by draw id; **bindless** resources |
| culling | CPU, parallel over archetype chunks, frustum spheres | GPU compute cull → compacted indirect args; **HZB occlusion**; BVH / cluster hierarchy |
| geometry LOD | discrete levels, screen-height selected, **cooker-generated** by vertex clustering; levels keep their material groups | the same, plus impostors or virtualised clusters, and QEM/silhouette-aware simplification |
| materials | per-draw uniform upload | material instance = index into a parameter buffer, bound once per pass |
| visibility granularity | per **item** (submeshes expanded at submit) | per drawable primitive / cluster |
| pass organisation | hardcoded shadow + opaque | render graph with automatic barriers and resource aliasing |
| threading | single submission thread | N command-recording threads |
| shadows | one 2048² map, one caster | cascades, cached static shadows, filtering |
| transparency | **none** (sort key has the layout, nothing feeds it) | sorted, OIT, or depth-peeled |
| post | **none** | HDR chain, TAA, bloom, tonemap |

What this renderer has that many do not, and which is worth protecting:

- a **GPU-free decision layer** (`src/render/world`) — cull, sort keys, light packing
  are pure functions over PODs, which is the only reason they are unit-testable at all;
- **submit counters** (`render/submit_stats.h`) — most engines cannot tell you their own
  material-bind count, and every performance claim in this repo is checked against them;
- a content-addressed cook cache with dependency invalidation;
- parallel extraction and cull, a stable radix draw sort, tagged heaps with a VRAM
  census.

## Measured: the bottleneck depends entirely on content shape

Synthetic (one mesh, one material, no submeshes, static):

| objects | extract | cull | shadow | submit | render | frame |
|---|---|---|---|---|---|---|
| 20 000 | 1.13 | 0.52 | 0.28 | 0.13 | ~2.1 | 8.2 (vsync) |
| 50 000 | 2.83 | 0.78 | 0.11 | 0.20 | 3.91 | 8.2 (vsync) |
| 100 000 | 5.41 | 1.37 | 0.15 | 0.36 | 7.29 | ~9.0 (111 fps) |

100 000 objects in **one draw call**; the bottleneck is extraction, which is CPU work
proportional to entity count and already parallel.

Real cooked content (~1.7 material groups per mesh):

| 5 000 props | |
|---|---|
| items / culled | 5 000 / 2 145 |
| draws | **2 869**, of which 2 811 from submeshes |
| instanced | 58 submits covering 966 items |
| material binds | 2 869 — one per draw |

At 50 000 real props the draw ceiling tripped and **the frame was incomplete** (draws
refused). That was the state that motivated this document; R18 fixed it (see the work
list below). The lesson survives the fix: the limit was the submission MODEL, not bgfx.

Every renderer number in this repo before 2026-08-04 was measured on cubes — the best
case on all four axes. That is the single most important thing to know when reading
older figures.

## The one real architectural debt

**Submeshes are expanded at submit time, outside the visible set and outside the sort.**
`VisibleDraw` is one entry per item; the submesh loop runs inside submission, after
sorting. Three consequences, one cause:

1. instancing is excluded by construction (`it.mesh->submeshes.empty()`), so 96 of the
   kit's 176 meshes can never instance — 50 000 real props produced **zero** instanced
   submits;
2. material-bind dedup cannot hit, because each item binds A, B, C for its own ranges
   and the only *correct* cache depth is one (bgfx holds a single set of uniform values,
   so skipping an upload for a material bound two draws ago renders it with the wrong
   values);
3. draw count inflates by the average material-group count.

Everything else in the table above is **unbuilt, not mis-built**. The evidence that the
foundation is sound is that this week's fixes were small and local: extraction
15.2 → 1.1 ms, cull 3.9 → 0.78, the interpolation snapshot 12.7 → 0.63, startup
2.60 → 1.16 s. Architectures that are wrong do not yield to local changes.

## A caveat about the stress scenes

50 000 full-detail props in a 1 km box with no LOD is not a workload a shipped game
presents. `gen_fuzz_scene.py` is deliberately adversarial: it exists to reach branches
that synthetic cubes never touch, and it has already found four real bugs. "We cannot
draw 50 000 props" overstates the practical problem. What it genuinely proves is that
the submission model is the limit.

## Order of work, and why

1. ~~**Submesh-granular visible sets.**~~ **DONE 2026-08-05** (issues.md R18). Ranges
   are expanded before the sort and carry their own material, so submeshed meshes
   instance and binds dedup: 50 000 real props went from 3 067 draws + 534 REFUSED (an
   incomplete frame) to **299 draws, 299 binds, 299 instanced submits covering 41 571
   items**, ceiling never reached. The submission wall described above is gone; the
   sections above are kept as the record of what it looked like. Extraction is now the
   bottleneck for real content too — 20.5 ms at 50 k, against 2.83 for cubes, because
   real content dereferences 176 distinct meshes, actually interpolates, and walks
   parents.
2. ~~**LOD.**~~ **BUILT** — selection 2026-08-06 (issues.md R20), generation 2026-08-08
   (`decimate.h`), and the review fixes 2026-08-10 (R21). Selection is screen-height based
   with the sort-key repair mutation-verified and an exact per-level census; generation is
   vertex clustering steered by triangle ratio, keeping each level's material groups so a
   multi-material prop does not change colour at a threshold.

   Worth keeping as a lesson: for two days this row was **built and worth nothing**,
   because selection shipped before anything could produce a cheaper mesh — every "level"
   had the parent's triangle count, so the correct, tested selection had nothing to
   select. A feature split across two subsystems is not half-delivered when the first
   half lands; it is undelivered, and the census said so only after it was taught to
   count triangles rather than entities.

   Note also that LOD was never going to move the CURRENT bottleneck, which is extraction
   (18.8 ms of a 24.8 ms CPU frame, vs GPU 9.11, at 50 k real props). It reduces GPU
   triangles and draws.
3. **Indirect submission — and bindless, which is a different question.** *(Split
   2026-08-28; this row previously read "Indirect + bindless submission… bgfx
   supports it", which is half wrong.)*

   **Indirect: bgfx supports it and we have never used it.** Compute dispatch,
   `createIndirectBuffer`, storage buffers and `submit(view, program,
   indirectHandle)` are all present in the vendored copy — with **zero call sites
   in this engine**. A compute cull writing compacted indirect args, with per-draw
   data in a storage buffer indexed by draw id, is expressible today. That removes
   the per-draw uniform payload and with it the 8 MB Metal ceiling.

   **Bindless: bgfx does not have it, at all.** `setTexture(stage, …)` per draw is
   the only model, and an indirect draw cannot bind anything — so a GPU-driven path
   over a scene with *per-object textures* is not reachable without leaving bgfx.
   A scene whose per-object variation is an index into a parameter buffer is.

   That split is the actual decision point for `docs/plans/rhi-design.md`: the
   indirect half is a spike we can run now, and the bindless half is the one that
   costs a backend.
4. **Multi-threaded encoders.** Free headroom once submission stops being the wall.

Not on this list, and deliberately: hand-written SIMD in the cull (measured — the plane
arithmetic is ~4% of the phase, so a 2.3x NEON win is 0.4% of the frame) and further
micro-optimisation of extraction (parallel already, so per-item savings divide by the
core count).

## The gap that is not about speed

**None of the rendering is visually verified.** Instancing, the shadow cull and shadow
instancing are confirmed by counters and timings on bgfx's Noop backend — never by
pixels. That is what keeps `src/render` at `tier: working` rather than `hardened`, and
no amount of further optimisation changes it. A real-device harness (golden image or
GPU timing) is the only thing that does.
