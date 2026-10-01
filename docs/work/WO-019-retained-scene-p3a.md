---
status: plan
id: WO-019
title: Retained scene P3a — table, ids, lifetime, rebuild-and-diff
program: renderer
priority: P2
size: L
state: todo
depends: [WO-020]
new-contracts: [render-scene]
touches:
  - src/render/world/render_world.h
  - src/render/renderer/extract.cpp
source: docs/plans/renderer-program.md P3; conversation 2026-09-22
---
## Why
Every frame the renderer rebuilds its draw list from scratch and copies a 64-byte matrix per visible object, even when nothing moved.

P3a builds the persistent table and proves it correct. It doesn't make anything faster yet. P3b to P3d are later orders, written when this lands.

## Done when
- [ ] SoA render tables in `src/render/world/` (GPU-free, so LAYER-04 holds), static and skinned as two instances of one type, with the columns of `renderer-program.md` §9.4. Bounds are the existing interleaved `CullSphere` stream, NOT planar AABBs: that layout was measured and reversed here (§9.4, WO-020)
- [ ] `RenderObjectId{index,generation}`: a stale id is detected, never aliased
- [ ] LIVE → RETIRED → FREE; a slot is reused only after its frame is fenced
- [ ] extraction writes the table wholesale each frame (no incremental path yet)
- [ ] the 18.8 ms extraction baseline re-measured on the current tree before anything is claimed against it (§9.8)
- [ ] **rebuild-and-diff mode**: rebuild from scratch and compare field by field against the maintained table, naming the entity and field on mismatch; runs in a test lane
- [ ] mutations: skipping the generation bump, and reusing a slot before its fence, each turn a test red

## Contract
`render-scene`: `create / update / destroy → RenderObjectId`, read by extraction.

Nothing: **null** on a headless server. `create` returns an invalid id, and `update`/`destroy` on it are no-ops, just like `NullRenderer`, so gameplay code never branches on "is there a renderer".

Ownership: the table owns all rows; ids are values and never pointers.
