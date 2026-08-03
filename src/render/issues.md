---
status: unreviewed
---
# Issues — renderer maintainability audit (Tue Aug 4)

Can `src/render` be maintained and reasoned about in isolation? Measured, not
asserted. **Verdict: the decision layer yes, the submission layer no.**

## What is genuinely well isolated

- **`world/` is bgfx-free.** Culling, sort keys, visibility and light packing
  mention bgfx only in comments explaining why it is absent. They compile and test
  without a graphics API (`render_world_test`), which is what makes the renderer's
  *decisions* reasonable in isolation.
- **`diag/` is payload-agnostic.** Churn, budget, census and frame stats take PODs
  and have their own test (`render_diag_test`).
- **`submit_stats.h` is a POD of counters** with a default all-zero implementation
  on the interface, so a pipeline that does not count still compiles.
- **Nothing outside `src/render` includes renderer internals.** The only external
  entry points are `renderer.h` and `render_pipeline.h`. That boundary holds.

## R10. ✅ FIXED — `forward_pipeline.h` was 812 lines, and a HEADER
The single worst maintainability problem in the subsystem, and it is one file:

| region | lines | concern |
|---|---|---|
| shader blob `#include`/`#define` per backend | ~130 | build plumbing, 3-way `#if` |
| `onAttach` | ~95 | program / uniform / render-target creation |
| `render()` | **274** | view uniforms, visible set, batch runs, per-item state, material binding (two branches), instancing, submesh loop, debug lines |
| `renderShadow()` | **147** | light matrices, light-space visible set, instancing, per-item fallback |
| `bind()`, draw ceiling, members | ~150 | |

Two consequences. `render()` at 274 lines cannot be read in one sitting, and it is
where every future change lands — instancing, the draw ceiling and the submit
counters all went in there this month. And being a **header** means the whole thing
plus 130 lines of platform `#if` recompiles into every translation unit that
includes it. Only `renderer.cpp` does today, so the compile cost is contained by
luck rather than by design.

The decomposition that follows the assetlib precedent (directory = layer, file =
concern):

    forward_pipeline.h              181  class declaration only
    pipeline/shader_blobs.h         113  per-backend #include/#define, ONE includer
    pipeline/programs.cpp           124  onAttach/onDetach: programs, uniforms, RTs
    pipeline/opaque_pass.cpp        296  render() + bind() + debug lines
    pipeline/shadow_pass.cpp        151  renderShadow()

DONE 2026-08-04, mechanically: every submit counter is byte-identical afterwards
(fps_shooter 10 items / 1 culled / 6 draws / 1 instanced covering 7; 2 000 cubes 283
culled / 1 draw / shadow 1 draw covering 244; 20 k exits 0; 34/34 unit).
`material_bind.cpp` and `draw_budget.h` from the original sketch were NOT created:
bind() is 17 lines used only by render(), and the ceiling guard is ~20 used by both
passes — splitting either would add a file without removing a concern.

## R11. ✅ FIXED (deleted) — `src/render/passes/` was a second, dead architecture
Nine headers — `i_render_pass.h`, `opaque_pass.h`, `shadow_pass.h`, `sky_pass.h`,
`transparency_pass.h`, `post_pass.h`, `resolve_pass.h`, `pass_context.h`,
`pass_list_pipeline.h` — plus `passes/docs/renderer-architecture.html`.

**Nothing includes them. They are in no CMake target. They do not compile.**

This is the direct answer to "can the renderer be reasoned about in isolation":
not while the tree contains two renderer designs and only one is live. A reader
who opens `passes/opaque_pass.h` expecting the opaque pass finds a plausible,
never-executed interface — and the names collide exactly with the decomposition
R10 wants (`opaque_pass`, `shadow_pass`).

Decide and act: either adopt it as the target architecture and migrate, or delete
it. Keeping it costs nothing to the compiler and a great deal to the next reader.
**Recommend deleting**, because `render()`'s real structure is now known from the
work of the last week and a pass list designed before any of it was measured is
unlikely to be the right shape.

## R12. `renderer.cpp` (437) mixes device lifecycle with ECS extraction ✅ FIXED
It owned bgfx init, framebuffers, view ids AND the flecs queries that build
`RenderItem`s. Extraction is the hot path (38 ms at 20 k objects, one draw call) and
the next optimisation target, so it wanted to be its own unit — testable against a
fake world rather than only through a live device.

Split four ways, the same shape as `pipeline/`:

| file | lines | concern |
|---|---|---|
| `renderer.cpp` | 83 | pipeline ownership: attach/detach, `makeContext` |
| `renderer/device.cpp` | 171 | bgfx up/down + the Rendering-heap allocator |
| `renderer/targets.cpp` | 125 | framebuffers + the three `render*` entry points |
| `renderer/extract.cpp` | 124 | ECS → `RenderView` — the hot path, alone |

Landed at `renderer/extract.cpp`, not the `render/extract.cpp` this issue named:
all four are `Renderer` methods, so they belong under a directory named for the
class, exactly as `forward_pipeline.h`'s TUs sit in `pipeline/`.

Two things changed beyond moving text, both to make `targets.cpp` about targets
rather than about framebuffer bookkeeping: `destroyTargets()` now holds the
six-handle teardown that `createSceneFB` and `shutdown` had each spelled out (the
resize leak this guards against is a real past crash — handle 65535 after a Scene
View drag), and `ensureGameFB()` pulls 20 lines of lazy creation out of
`renderGameView`. Verified by identical submit counters at 2 000 and 20 000
objects, and 48/48 ctest including the stress and soak lanes.

What this does NOT do is make extraction testable without a device: `buildView`
is still a private method reading five borrowed registries. It is now the only
thing in its file, which is what the optimisation work needs; a seam for a fake
world is a separate change, and R13's headless pipeline test is the better place
to force it.

## R13. `tier: prototype` is still correct, and now for a narrower reason
Covered: `GpuResourceCache`, the three registries, `world/`, `diag/`, shader
selection. Not covered: submission. The counting seam makes a headless pipeline
test possible for the first time (bgfx Noop never sets `numDraw`; our counters do),
and writing it is the single thing that raises this tier.

## Not a defect, but the thing to know before touching the pipeline
Per-draw uniform bytes are a hard budget, not a soft cost: bgfx's Metal backend
commits them into a fixed 8 MB buffer with no bounds check, so ~8192 draws
segfaults. `kMaxDrawsPerFrame` guards it. Any change that adds a per-draw uniform
lowers that ceiling, and instancing is what raised it.
