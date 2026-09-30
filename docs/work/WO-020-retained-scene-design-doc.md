---
status: plan
id: WO-020
title: Write the retained-scene design into the renderer programme (§9)
program: renderer
priority: P2
size: M
state: todo
touches:
  - docs/plans/renderer-program.md
source: conversation 2026-09-22 — retained-scene design, VRAM-resident materials, five-engine survey
---
## Why
The retained-scene design, the five-engine comparison and the "material data lives in VRAM" rule exist only in chat.

Code written before this is on disk would be built from memory.

## Done when
- [ ] §8.6: the five-engine comparison, closing §8.5's owed items
- [ ] §9: SoA table in `render/world/`; `RenderObjectId{index,generation}`; LIVE → RETIRED → FREE, retired by frame fence; `OnSet` dirty tracking with ordinary vs structural changes; one apply point before the three `buildView` calls; per-batch contiguous slots; sparse uploads with a *measured* crossover; planar AABBs; rebuild-and-diff debug mode
- [ ] §9 rule: **the CPU reads an object's identity and bounds, never its contents.** Transforms, material parameters and texture bindings are fetched by the GPU through indices (`BUFFER_RO` in the vertex shader, proven possible on bgfx by example 41-tess).
- [ ] §9 is honest about limits: the visible-list compaction and LOD selection stay O(visible)/O(total) until GPU-driven
- [ ] open questions #7, #12 and #13 are each answered or explicitly deferred with a reason. #13 (one render table or two for skinned instances) is a runtime-layout question; it is NOT the import question WO-009 decides (see docs/plans/imported-scene.md §3).
