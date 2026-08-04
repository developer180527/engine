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
`RenderItem`s. Extraction is the hot path (measured at 15.2 ms of a 27.1 ms frame
over 20 000 objects — see R14, which the split made possible to find) and the next
optimisation target, so it wanted to be its own unit — testable against a fake
world rather than only through a live device.

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

## R13. `tier: prototype` is still correct, and now for a narrower reason ✅ FIXED
Covered: `GpuResourceCache`, the three registries, `world/`, `diag/`, shader
selection. Not covered: submission. The counting seam makes a headless pipeline
test possible for the first time (bgfx Noop never sets `numDraw`; our counters do),
and writing it is the single thing that raises this tier.

`tests/render_pipeline_test.cpp` — 8 cases, 25 assertions, `tier: working`. It
builds a `RenderView` by hand: no ECS, no `Renderer`, no window, because
`RenderView` is a struct of spans and `RenderContext` is three registry
references. That is the whole reason this is a unit test and not an engine boot.

What it asserts, and why each one: an empty view submits nothing; items behind the
camera are culled (the cull is *wired*, not merely unit-tested in `rworld`); 64
identical draws collapse to one batch run and one instanced submit; **eight
distinct materials do NOT** collapse — the negative case, because if the sort key
stopped separating materials, instancing would render with the wrong one; the draw
ceiling caps at 4096 and *reports* the 1904 it refused; one bone-palette upload for
a 4-submesh skinned item (R4, as an assertion instead of a printed warning); the
shadow pass counts separately and submits nothing when `castShadows` is off; and
the counters reset per view — which every other assertion depends on.

MUTATION-CHECKED, because a passing test proves nothing until it can fail:
removing `render()`'s `m_submitStats.reset()` fails 8 assertions, and disabling the
instanced path (`runLen > 1` → `> 100000`) fails 3. Both restored.

Two things fixed on the way. `ForwardPipeline::submitStats()` was declared
*private* while `IRenderPipeline` declares it public — legal (an override may
narrow access) but it meant the counters were reachable only through a base
pointer, including for the test whose entire purpose is reading them. And the
first run of the test died inside `bgfx::setVertexBuffer` on handle 65535: the
fixture held `std::vector<Mesh>` while `RenderItem` holds a raw `Mesh*`, so a
reallocation dangled every item built earlier. Stable storage now, with the reason
recorded in the fixture.

Still NOT covered, so this is `working` and not `hardened`: pixels. Noop executes
nothing, so instancing, the shadow cull and shadow instancing remain verified by
counts and timings and never visually.

## R14. Extraction's cost was per-entity component lookups, not maths ✅ FIXED
The whole render path had ONE profiler zone (`"Render"` in runtime_frame.cpp), so
"38 ms at 20 k objects, all of it extraction" — repeated several times in this
repo's history, including by me — was **never measured**. It was frame time with an
assumed cause. Adding `Render.extract` / `.cull` / `.shadow` / `.submit` zones gave
the real split at 20 000 objects:

| phase | before | after |
|---|---|---|
| `Render.extract` | 15.2 ms | **9.7 ms** |
| `Render.cull` | 1.6 | 1.7 |
| `Render.shadow` | 0.8 | 0.8 |
| `Render.submit` | 0.08 | 0.09 |
| frame | 27.1 ms | **17.8 ms** |

Cadence after the fix is p50 17.75 ms (54.9 fps). The pre-fix cadence was never
captured — only the 27.1 ms frame cost — so there is no honest before/after fps
pair to quote, and the frame-time row is the comparison that was actually measured.

Then a differential run — one build, an env-var per suspect — attributed
extraction itself. The result was not what reading the loop suggests:

| suspect | cost | verdict |
|---|---|---|
| `try_get<PrevTransform>` + interpolation | ~6.3 ms | **fixed** — optional query term |
| `target(ChildOf)`+`is_alive`+`has<Transform>` | ~2.2 ms | **fixed** — two queries |
| `try_get<Transform>`, redundant | ~1.5 ms | **fixed** — query already had it |
| three registry lookups | ~0.17 ms | *not worth touching* |
| `m_items` growth (reserve) | ~0.4 ms | *not worth touching* |

Almost none of it was arithmetic. It was per-entity lookups asking questions the
query engine answers **per archetype**: does this entity have a parent (no, 20 000
times), does it have a PrevTransform, and give me the Transform the iteration is
already holding. So the fix is in the query *declarations*, not the loop body:
items are matched by two queries partitioned on `ChildOf`, and `PrevTransform` is
an optional term. Parented entities still get their full ancestor walk, so this is
a pure speedup — every submit counter is byte-identical at 1 k, 5 k and 20 k.

`getWorldMatrixLerp` gained `localMatrixLerp` + `getWorldMatrixLerpFrom` overloads
taking components already in hand; the entity-only version stays for callers that
genuinely only have an entity. `WorldQueryCache::get` gained a builder-customiser
overload so a cached per-world query can carry extra terms.

Two of my own claims corrected in the process, both now recorded at the code: the
"38 ms" was frame time and not extraction, and my first guess that the parent
lookup was 8.5 ms of it was wrong — it is 2.2, and PrevTransform was the big one.

TESTED by `tests/extract_partition_test.cpp`: the partition has exactly one
failure mode and it is silent — an entity in neither half vanishes from the frame,
one in both is drawn twice, and both read as content bugs. It asserts the two
queries cover the single query exactly with no overlap (including a depth-2
grandchild), that a parentless entity's fast path equals the full walk, that a
parented one still applies every ancestor (including through an ancestor with no
Transform), and that passed-in vs looked-up PrevTransform interpolate identically.
Scope limit, stated at the test: the matrix assertions run production code, but the
partition assertions build their own queries, so they prove the technique rather
than that `Renderer::init` uses the right term. Byte-identical submit counters at
1 k / 5 k / 20 k are the engine-level evidence for that.

REMAINING: 9.7 ms for 20 000 items is ~0.5 µs each, and what is left is real work
(quaternion nlerp, matrix compose, push_back). The next win is width — SIMD or jobs
over archetype spans — not more lookup removal. `Render.submit` at 0.09 ms means
submission is 0.7% of the render path; it is finished as an optimisation target.

## R15. Extraction, continued: 6.6 ms of the remaining 9.7 was still avoidable ✅ FIXED
R14 got extraction from 15.2 ms to 9.7 ms at 20 000 objects and said the next lever
was "width — SIMD or jobs". Half right. Three more changes, in the order they were
measured:

| change | extract @20k |
|---|---|
| (after R14) | 9.7 ms |
| `Transform::getMatrix` composes SRT directly | 8.0 ms |
| `SkinnedMesh` as an optional query term | 6.6 ms |
| chunked extraction on the job pool | **1.1 ms** |

**getMatrix was doing two full 4x4 multiplies** — 128 multiplies, 96 adds — to
compose an SRT where S is diagonal and T is the identity with one row replaced.
Three quarters of that arithmetic was against structural zeros. Written out
directly it is the same 16 floats, and this is the most-called function in the
engine (every entity every frame, plus physics sync, animation, gizmos). Verified
BIT-EXACT against the two-multiply reference over 2 000 randomised transforms
including negative, non-uniform and zero scale (worst element delta 0.0), and the
`m[12..14] == position` contract the gizmo depends on is asserted separately.

**SkinnedMesh** was the last per-entity `try_get` in the loop, worth ~1.4 ms — and
removing it had a second effect that mattered more than the time: the per-item body
now touches NO ECS handle, only component arrays. That is what made the parallel
step a safety argument about disjoint slices rather than about flecs thread rules.

**Chunked jobs**: extraction slices each matching archetype into 512-item ranges of
contiguous arrays (walking TABLES, so 20 000 objects in one archetype is a few dozen
chunks, not 20 000 iterations), then `jobs::parallelFor` fills disjoint slices of a
pre-sized `m_items`. Items whose mesh is missing leave gaps, which a compaction pass
closes — skipped entirely when nothing was dropped, i.e. every normal frame.

ONE BODY serial or parallel. Below 2 048 items, or with no job pool (headless tools,
tests), the same `extractChunk` runs in a loop. Two implementations would mean the
tests exercise one and the shipped engine the other, and the drift would be silent.

This retires the runtime's "single-threaded frame orchestration" tradeoff in part —
`engine::jobs` over extraction was its stated trigger.

SIMD was NOT written, deliberately. The remaining per-item maths is spread across
cores, and the frame's cost has moved somewhere else entirely: at 50 000 objects the
frame is 34 ms of which the whole renderer is 8.6 (extract 2.6, cull 3.9, shadow 1.8,
submit 0.3). The other ~25 ms was `Sim.prevSnapshot` — a deferred structural `set<>`
per entity per fixed step, which had no profiler zone and so was invisible until now.
Fixed the same way (`src/runtime/docs/issues.md` H.0: 12.7 ms -> 0.63 per step), which
leaves the 50 000-object frame at 9.3 ms with the renderer 8.4 of it — and CULL, not
extract, the largest render phase. Hand-optimising this
file further would be optimising a quarter of the problem.

NOT sanitizer-verified at scene scale: ASan on the windowed host runs ~13 s per frame
here, so that run was abandoned rather than left to burn. What stands behind the
parallel path: byte-identical submit counters serial vs parallel at 1 k / 5 k / 20 k /
50 k (and those counters depend on every matrix, because culling reads them), a debug
assert on the slice arithmetic, and the render + sim tests passing under ASan. A TSan
lane over a scene this size is still worth having and is not yet there.

## Not a defect, but the thing to know before touching the pipeline
Per-draw uniform bytes are a hard budget, not a soft cost: bgfx's Metal backend
commits them into a fixed 8 MB buffer with no bounds check, so ~8192 draws
segfaults. `kMaxDrawsPerFrame` guards it. Any change that adds a per-draw uniform
lowers that ceiling, and instancing is what raised it.
