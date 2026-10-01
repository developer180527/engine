---
status: plan
id: WO-020
title: Write the retained-scene design into the renderer programme (§9)
program: renderer
priority: P2
size: M
state: done
done: 2026-10-01
evidence: renderer-program.md §8.6 (five engines, sourced) and §9.4–9.11; every code and bgfx claim re-checked against the tree on 2026-10-01; #7 and #13 decided, #12 deferred to P3b with its reason; the 2026-09-22 conversation recovered from its transcript, not recalled
touches:
  - docs/plans/renderer-program.md
  - docs/rhi/phases.md
  - docs/work/WO-019-retained-scene-p3a.md
source: conversation 2026-09-22 — retained-scene design, VRAM-resident materials, five-engine survey
---
## Why
The retained-scene design, the five-engine comparison and the "material data lives in VRAM" rule exist only in chat.

Code written before this is on disk would be built from memory.

## Done when
- [x] §8.6: the five-engine comparison, closing §8.5's owed items
- [x] §9: SoA table in `render/world/`; `RenderObjectId{index,generation}`; LIVE → RETIRED → FREE, retired by frame fence; `OnSet` dirty tracking with ordinary vs structural changes; one apply point before the three `buildView` calls; per-batch contiguous slots; sparse uploads with a *measured* crossover; planar AABBs; rebuild-and-diff debug mode
- [x] §9 rule: **the CPU reads an object's identity and bounds, never its contents.** Transforms, material parameters and texture bindings are fetched by the GPU through indices (`BUFFER_RO` in the vertex shader, proven possible on bgfx by example 41-tess).
- [x] §9 is honest about limits: the visible-list compaction and LOD selection stay O(visible)/O(total) until GPU-driven
- [x] open questions #7, #12 and #13 are each answered or explicitly deferred with a reason. #13 (one render table or two for skinned instances) is a runtime-layout question; it is NOT the import question WO-009 decides (see docs/plans/imported-scene.md §3).

## Log
- 2026-10-01:
  - **Written from the source, not from memory.** The 2026-09-22 conversation
    is on disk (a session transcript). The survey, the design, and the
    revision after the "material data is in VRAM" correction were taken from
    it, and every claim about the code or bgfx was re-checked against today's
    tree first:
    - the 64-byte per-instance copy (`opaque_pass.cpp`);
    - `buildView` called three times a frame (`targets.cpp`);
    - the CPU material walk (`bindMaterial`);
    - `setInstanceDataBuffer(DynamicVertexBufferHandle, ...)` and
      `update(DynamicVertexBufferHandle, ...)` in the vendored `bgfx.h`;
    - `BUFFER_RO` in `41-tess`'s vertex shader, bound before `submit`;
    - no bindless capability in bgfx.
  - **One done-criterion overruled by evidence: planar AABBs.** The survey
    suggested them (Filament) and this order listed them. This tree had
    already measured four parallel arrays against one interleaved stream for
    its cull spheres and reversed it: interleaved is 1.27x faster, and the
    arithmetic planar speeds up is ~4% of the cull. §9.4 keeps the measured
    layout and says why; WO-019's criterion is changed to match.
  - **The check the conversation owed, done.** Can material parameters sit at
    a fixed stride in VRAM? Yes, per shader: cooked blocks are built from the
    shader's declared parameter list, complete and never sparse. So it is one
    material buffer per shader.
  - **Stated as limits, not glossed:**
    - bgfx has no bindless, so the texture half of §9.5's rule is met only
      under the RHI;
    - the visible list and LOD selection stay O(visible) and O(total) until
      G6;
    - the 18.8 ms baseline predates the LOD work, and P3a re-measures it.
  - **Questions:**
    - #7: decided for what exists. A pending mesh draws the placeholder
      (WO-018), and residency eviction never evicts a referenced mesh. A
      `streamedOut` flag is reserved for real streaming.
    - #12: deferred to P3b. It is a measured number, and P3a changes no
      upload.
    - #13: two tables of one type. Skinned rows are always dirty and are
      already excluded from instancing.
  - **§8.5 is not fully closed, and says so.** Decima is placed (it informs
    the visible list, not P3) but its slides are still unread. The
    Haar/Aaltonen and RAGE items are unchanged: a retained-scene survey does
    not answer them.
  - **Renumbering.** §9 runs 9.1–9.11 in reading order. The exit curve moved
    from §9.6 to §9.10, and both references to it (this document's P3 row,
    `docs/rhi/phases.md`) were updated. §9.2, which `imported-scene.md` cites,
    kept its number.

