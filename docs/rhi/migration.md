---
status: plan
covers:
  - src/render/
---
# Migration: strangle, never rewrite

*Original `rhi-design.md` §7. Split out 2026-09-04; wording preserved.*

## 7. Migration

The seam exists; use it.

1. **Close the seam properly.** Purge bgfx from the files outside `src/render/`,
   and get `bgfx::TextureHandle`/`ViewId` out of `RenderContext` behind opaque
   engine handles. *This is the headless-server refactor too.*
   ([`evidence-coupling.md`](evidence-coupling.md) §2.1 has the corrected counts —
   the graphics problem is five files, not thirty.)
2. **`modules/rhi/`** as a sibling of `modules/assetlib/` — its own library, its
   own tests, no engine dependencies.
3. **`GpuDrivenPipeline : IRenderPipeline`** on `rhi`, selected by a project flag.
   `ForwardPipeline` on bgfx keeps working the entire time.
4. **A/B on the farm** — same scene, same seeds (`gen_fuzz_scene.py`), submit
   counters and GPU time side by side. This is how SDL3-vs-GLFW and
   scalar-vs-NEON were decided in this repo; it is the house method.
5. **Delete bgfx only when the new path wins on real hardware**, on a real scene,
   with the tests green.

At no point is `main` broken, and at every point the thing can be abandoned with
the cleanup work already banked.

## Why step 2 says `modules/`, not `src/`

`modules/assetlib/` is the precedent: a library with its own tests and no engine
dependencies, which is what made it usable by the cook worker as well as the
runtime. The RHI has the same requirement for a stronger reason — vCAD is a second
consumer, and a second consumer cannot depend on `src/runtime/`.

This is also what [`../plans/renderer-program.md`](../plans/renderer-program.md)
§1 means by the reuse boundary sitting *below* the renderer rather than through
it. `modules/rhi/` is that boundary made into a directory.
