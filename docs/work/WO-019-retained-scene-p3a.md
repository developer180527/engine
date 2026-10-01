---
status: plan
id: WO-019
title: Retained scene P3a — table, ids, lifetime, rebuild-and-diff
program: renderer
priority: P2
size: L
state: done
done: 2026-10-01
evidence: render_scene_test (ids, fence, apply, diff; skipping the generation bump fails 2 checks, reusing a slot before its fence fails 4); diff mode green over 900 frames of a 50k real-content scene; Render.extract re-measured 4.0-4.6 ms (was 18.8 in R20), the table costs 0.46 ms; 127 tests (all but the 10 fuzz explore campaigns)
depends: [WO-020]
new-contracts: [render-scene]
touches:
  - src/render/world/render_scene.h
  - src/render/world/render_scene.cpp
  - src/render/renderer/extract.cpp
  - src/render/renderer.h
source: docs/plans/renderer-program.md P3; conversation 2026-09-22
---
## Why
Every frame the renderer rebuilds its draw list from scratch and copies a 64-byte matrix per visible object, even when nothing moved.

P3a builds the persistent table and proves it correct. It doesn't make anything faster yet. P3b to P3d are later orders, written when this lands.

## Done when
- [x] SoA render tables in `src/render/world/` (GPU-free, so LAYER-04 holds), static and skinned as two instances of one type, with the columns of `renderer-program.md` §9.4. Bounds are the existing interleaved `CullSphere` stream, NOT planar AABBs: that layout was measured and reversed here (§9.4, WO-020)
- [x] `RenderObjectId{index,generation}`: a stale id is detected, never aliased
- [x] LIVE → RETIRED → FREE; a slot is reused only after its frame is fenced
- [x] extraction writes the table wholesale each frame (no incremental path yet)
- [x] the 18.8 ms extraction baseline re-measured on the current tree before anything is claimed against it (§9.8)
- [x] **rebuild-and-diff mode**: rebuild from scratch and compare field by field against the maintained table, naming the entity and field on mismatch; runs in a test lane
- [x] mutations: skipping the generation bump, and reusing a slot before its fence, each turn a test red

## Contract
`render-scene`: `create / update / destroy → RenderObjectId`, read by extraction.

Nothing: **null** on a headless server. `create` returns an invalid id, and `update`/`destroy` on it are no-ops, just like `NullRenderer`, so gameplay code never branches on "is there a renderer".

Ownership: the table owns all rows; ids are values and never pointers.

## Log
- 2026-10-01:
  - **The table.** `src/render/world/render_scene.h/.cpp`, GPU-free and
    runtime-free (LAYER-04 holds).
    - `RenderObjectId{index, generation}`. The generation bumps at retire, so
      an id is dead the moment its object is.
    - `RenderTable` holds the SoA columns of `renderer-program.md` §9.4.
      Bounds are the interleaved `CullSphere`, not planar AABBs (WO-020).
    - The state machine is LIVE → RETIRED → FREE. `collect(completedFrame)`
      frees only slots whose retire frame has completed.
    - `RenderScene` holds a static and a skinned table of one type,
      `apply()` (wholesale), and `diff()` (rebuild and compare).
    - Contract `render-scene`, with `NullRenderScene` as the null.
  - **In the renderer.** Extraction captures each item's view-independent row
    (level 0, before LOD) on the first view of a frame for a world, which is
    the single apply point. The chunks carry the archetype's entity ids for
    it. `endFrame()` advances the fence clock. The play world's scene dies in
    `resetWorldCaches`. It is opt-in (`ENGINE_RENDER_TABLE=1`, or `_DIFF=1`
    to rebuild and diff each frame, aborting on the first mismatch), because
    nothing reads the table until P3c. Off, the hot path pays a branch per
    chunk.
  - **Measured** on `gen_fuzz_scene --objects 50000 --seed 1` (176 real
    meshes, 12 506 movers, 7 550 parented), with `build-prof` and a windowed
    `engine_host --frames 900`:

    | | `Render.extract` | `Render.table` |
    |---|---|---|
    | table off | 4.0–4.6 ms | — |
    | table on | 4.5–5.0 ms | 0.46 ms |
    | diff mode | 9.1–9.4 ms | 4.1–4.2 ms |

    Diff mode stayed green for all 900 frames.
  - **The baseline finding.** R20's 18.8 ms is now 4.0–4.6 ms on the same
    scene shape, about 4x less. The work since R20 removed most of the
    bottleneck P3 was argued from. Recorded in `renderer-program.md` §3 and
    §9.8, and in R20 itself. Whether that changes P3b–P3d's priority is the
    owner's call; the case should now be made on the §9.10 curve at 100k to
    500k objects.
  - **What P3a's diff can and cannot catch.** The table is written from the
    same capture it is compared with, so in P3a the diff proves lifetime
    bookkeeping: missing, leaked, stale and mis-homed rows (all pinned by
    `render_scene_test` §4). Missed change hooks are what it catches once P3b
    writes incrementally.
  - **Not covered at runtime: skinned rows.** No existing scene drives skinned
    draws through a host. `fps_shooter`'s last scene has 7 items, and a
    generated zombie scene loads as plain meshes or as placeholders, because
    the fuzz project ships only cooked files. The skinned table's logic is
    unit-tested (two tables, moving a row between them, the diff naming the
    wrong table). P3b's gate now requires an animated scene.

