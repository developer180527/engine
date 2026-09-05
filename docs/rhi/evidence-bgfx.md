---
status: as-built
verified: 2026-09-05
covers:
  - src/render/
---
# What bgfx costs us, and what it does not

*Original `rhi-design.md` §1 and §2.2. Split out 2026-09-04; wording preserved.*

> Rungs 1 and 2 on [`workflow.md`](workflow.md) §1 — everything here is measured
> or counted in this tree, not argued. `verified:` is the date the counts in §2.2
> were taken.

## 1. What bgfx actually costs us, measured

Not opinions — findings already in the tree:

| Cost | Evidence |
|---|---|
| **Per-draw uniform payload is a hard ceiling.** ~1 KB per draw into Metal's fixed 8 MB scratch buffer **with no bounds check**, so ~8192 draws segfaults inside `commit`. `kMaxDrawsPerFrame = 4096` exists to guard a missing check in someone else's backend. | `docs/architecture/renderer-vs-production.md` |
| **No bindless.** `setTexture(stage, …)` per draw is the only model. This is what blocks GPU-driven submission, because an indirect draw cannot bind anything. | API surface |
| **No ray tracing at all.** Not exposed, not in the abstraction's vocabulary. | — |
| **No explicit barriers, queues, or timeline semaphores.** So no async compute for culling or BVH builds, no copy-queue streaming. | — |
| **Threaded submission measured WORSE.** Two of three runs stalled ~1 second on Metal drawable acquisition, and pipelining costs a frame of latency against the motion-to-photon budget this engine exists for. We deliberately run single-threaded. | `src/render/renderer/device.cpp` |
| **Resource-pool walls we pay for in engine design.** `BGFX_CONFIG_MAX_*_BUFFERS = 4096` once dropped 92% of a scene. | `src/assets/issues.md`, `tests/mesh_dedup_test.cpp` |

And what bgfx is **not** costing us, which matters just as much for honesty:

- it is not the frame's bottleneck (extraction is);
- the 4 096 draw ceiling is *ours*, guarding *their* missing check — bgfx's own
  limit is 65 535;
- 50 000 real props already submit in **299 draws** with 299 material binds, after
  submesh-granular visible sets and instancing (R18). The submission model was the
  wall, and we already moved it a long way inside bgfx;
- it has ~15 years of driver-bug workarounds we currently get for free. **This is
  the real cost of leaving, and it is not an API-design problem.**

## 2.2 The headroom we have never touched

Measured 2026-08-28, and it belongs in this document because it changes what
G0 should be:

> **bgfx has compute dispatch, indirect buffers, storage buffers and
> `submit(view, program, indirectHandle)`. This engine has ZERO call sites for
> any of them.**

`renderer-vs-production.md` says the same thing from the other side — "compute
shaders, indirect draws, multi-threaded encoders, instancing; we use one of those
four" — and instancing is the one.

So the claim "bgfx cannot give us GPU-driven rendering" is currently **untested**.
What bgfx genuinely lacks is **bindless**, and that is what blocks GPU-driven
rendering *for a textured game scene*, because an indirect draw cannot bind a
texture per draw. It does not block a compute cull writing indirect args, nor
per-instance data read from a storage buffer.

That distinction is worth a spike before committing a year: a GPU-driven cull →
indirect draw path on bgfx, with per-instance material indices in a storage
buffer, would either move the 24.8 ms extraction number or fail against a wall we
can name precisely. Either outcome is worth more than the rest of this directory
is without it. That spike is [`phases.md`](phases.md) **G0a**, and it is
[`studies/`](studies/) question 004.

~~The seam we need also half-exists.~~ **Closed 2026-09-04 (G1b).**
`RenderContext` used to hand pipelines `bgfx::TextureHandle` and `bgfx::ViewId`,
so the seam leaked and could not have carried a second backend. It now carries
`gpu::` types, and `tests/headless_include_probe.cpp` compiles a third-party
`IRenderPipeline` with bgfx absent from the include path.

What remains true is the other half: `IRenderPipeline` still has **one
implementation**, and one implementation is how a swap point decays. The seam is
now *able* to carry a second backend; nothing has yet proved that it does.
