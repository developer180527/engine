---
status: target
covers:
  - src/render/
---
# The API, the frame, and what survives

*Original `rhi-design.md` §4.2–4.4. Split out 2026-09-01; wording preserved.*

> `status: target` — this describes intended design. Nothing here exists.

## 4.2 The core API, concretely

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

## 4.3 What the frame looks like

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
everything — which is the fix for the 18.8 ms that this whole directory started
from, and it is not reachable through bgfx.

That first line is the hardest one and the least designed. "Only DIRTY entities
re-uploaded" is a retained scene with an ownership model, and how shipped engines
solved it — Unreal's proxy plus a single apply point, Unity's `HeapAllocator` and
`SparseUploader` — is [`studies/`](studies/) question 003.

## 4.4 What we keep

More than one might expect, and this is the argument for the migration being
survivable:

- **`rworld::` stays, and stays GPU-free.** Sort keys, LOD selection, light
  packing, frustum math are pure functions over PODs. LOD selection in particular
  is *already* the right shape to run per-instance in a compute shader.
- **The whole asset and cook layer stays.** The DDC, content-addressed cooking,
  the `.cooked` formats, LOD decimation (v5). Only the shader cooker's back end
  changes.
- **`rdiag::SubmitStats` stays and gets more important** (see
  [`testability.md`](testability.md)).
- **`IRenderPipeline` stays** as the A/B seam.
