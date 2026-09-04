---
status: target
covers:
  - src/render/
---
# Ray tracing — designed now, built late

*Original `rhi-design.md` §4.5. Split out 2026-09-01; wording preserved.*

## 4.5 Ray tracing, specifically

Worth designing now because it constrains the resource model, not because it is
early:

- **Inline ray queries first** (`DXR 1.1` / `VK_KHR_ray_query`) — no separate
  shader stages, no shader-binding-table machinery. Cheapest useful path: RT
  shadows (which sidesteps the cascaded-shadow-map project entirely), RTAO.
- **Non-obvious payoff for an FPS: gameplay queries.** Line-of-sight, AI
  visibility and audio occlusion against the same BVH the renderer uses, on the
  async compute queue. That is a genuine differentiator and it costs almost nothing
  once the TLAS exists.
- **What it will actually cost:** BLAS per LOD level (interacts directly with the
  decimation work), per-frame BLAS refit for skinned meshes, TLAS rebuild
  per frame, and a real VRAM line item. Alpha-tested geometry needs any-hit
  shaders, which is the same problem as our current "alpha-tested casters cast
  solid shadows" gap (R19), solved properly instead of worked around.

## The constraint this places on the floor

Hardware ray tracing is **not** available on the stated minimum spec — see
[`open-decisions.md`](open-decisions.md) decision 3. So G7 is an **optional
tier** with a working path when absent, not a baseline. That was not true of the
original document, which assumed a Turing+/RDNA2+ floor that contradicted
`renderer-architecture.md`'s acceptance test.

The iPad floor is a third number again, set by argument buffers and
`MTLIndirectCommandBuffer`, and it has to be stated before G7 rather than
discovered in it — [`studies/`](studies/) question 005.
