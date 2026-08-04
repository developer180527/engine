---
status: as-built
tier: working
verified: 2026-08-04
covers:
  - src/render/world/
tests:
  - tests/render_world_test.cpp
---
# Render world — the machinery half of the renderer


## Two frusta, one extractor
`extractFrustumPlanes` turns a view*proj into six inward normalized planes. It is
shared because a frame has more than one frustum: the camera's (built in
`Renderer::buildView`) and the directional light's (built in
`ForwardPipeline::renderShadow`). The shadow pass needs its OWN planes — an object
behind the camera can still cast a shadow into view, so culling shadow casters
against the camera would delete real shadows. Before this existed the shadow pass
had no planes and culled nothing.

**Culling depends on meshes having bounds, and a whole class of them did not.**
`hasBounds()` is false when `boundsMin/Max` are still ±infinity, and the frustum
test reads that as "never cull" — the safe direction, since a mesh that could be
anywhere must not vanish. But `PrimitiveLibrary` never set them, so every cube,
sphere and plane was silently exempt from culling in both passes. Fixed at the
source (bounds computed from the vertices at upload). If a new mesh source ever
appears, this is the thing to remember to set.

## Purpose
Separate **what the frame looks like** from **how the frame gets drawn**.

`IRenderPipeline` has always been advertised as the customization point: swap it
and your game looks different. It was not usable as one. Culling, sort order,
batching and light packing all lived *inside* `ForwardPipeline`, so a studio that
wanted a cartoon look — different shading, nothing else — had to reimplement
frustum culling to get it. That is the gap this directory closes
(`docs/architecture/renderer-architecture.md` §3, §5).

Everything here is **GPU-free**. Nothing includes bgfx; nothing dereferences
`Mesh`. That is a hard constraint, not a stylistic one, and it buys two things:

- **Testability.** `src/render` has no GPU harness, which is exactly why its
  pipeline was tier `prototype` with no test that could fail. Culling and sort
  order are the two things in a renderer that fail *silently* — geometry pops out
  of existence at the screen edge, batching quietly collapses to one draw per
  object, and neither crashes. `tests/render_world_test.cpp` asserts both.
- **Reuse.** A replacement pipeline calls `buildVisibleSet` and `packLights` and
  spends its effort on shading instead.

## Files — one concern each

| File | Concern |
|---|---|
| `render_world.h` | The frozen frame description: `Span`, `RenderItem`, `LightItem`, `ViewCamera`, `RenderWorld`. No behaviour. |
| `frustum.h/.cpp` | Is this bounding volume outside the view? |
| `sort_key.h` | Turn "how should this draw be ordered?" into one `uint64_t`. |
| `visibility.h/.cpp` | Cull → key → sort → `VisibleSet`, plus the batch-run predicate. |
| `draw_sort.h/.cpp` | Order the draw list by key: a stable, data-adaptive radix sort. |
| `cull_stream.h` | One RenderItem → the 24 bytes the cull reads (`writeCullEntry`). |
| `light_packing.h/.cpp` | `LightItem[]` → the float layout the shader expects. |

## The cull reads streams, not items

`RenderItem` is 144 bytes and the cull used to touch offsets 0..141 — all three of
its cache lines — to recover a bounding sphere and two ids. `CullStreams`
(render_world.h) is the SoA alternative: parallel `x/y/z/r/keyBase` arrays, 24 bytes
per item, filled by extraction while the model matrix is still in registers.

The bigger reason is not the byte count. **The world bounding sphere does not depend
on the camera**, yet it was rebuilt per view — once against the camera frustum and
again against the light's. Building it once at extraction cut `Render.shadow` from
0.25 ms to 0.09 at 50 000 objects, because the shadow pass now reads the same stream
instead of redoing the work.

Two encodings worth knowing before touching it. The radius carries both sentinels, so
the hot loop needs no side table: `r < 0` means the item is not renderable, `r ==
infinity` means renderable but unbounded (never culled — an unbounded mesh is a
missing-data problem, and dropping it silently would read as a renderer bug). And the
sort key is split: extraction packs material+mesh (`opaqueKeyBase`), the cull ORs in
depth (`withOpaqueDepth`), and `render_world_test` asserts the split form equals the
single-shot `makeSortKey` over randomised inputs — a one-bit disagreement would
reorder draws and break batching while producing perfectly plausible keys.

`buildVisibleSet` REQUIRES the streams; there is no fallback that rebuilds them from
the items. A fallback would be the path the tests exercise while the fused one ships.
Callers that are not extraction use `rworld::fillCullStream`, which goes through the
same `writeCullEntry`.

## Threads, without depending on the runtime

The frustum test is the hot loop here (`Render.cull` was the largest phase in the
render path at 50 000 objects), so it runs on the job pool — but this directory does
**not** include `runtime/jobs/jobs.h`. Being GPU-free *and* runtime-free is the whole
reason everything here is unit-testable, and spending that to get threads would be a
bad trade. `buildVisibleSet` takes an injected `rworld::ParallelForFn` instead;
`ForwardPipeline` supplies one backed by `jobs::parallelFor`, and `nullptr` runs
serially.

That indirection also bought the test that matters. `render_world_test` runs one
world four ways — serial, one range at a time, ranges in **reverse**, and one OS
thread per range — and demands byte-identical draw lists. Identical rather than
merely equivalent, because a survivor is written at its own item index rather than
appended: no two ranges touch the same memory, and surviving order is item order
however the work was scheduled.

`sortDraws` is **stable**, unlike the `std::sort` it replaced. Draws with equal keys
keep item order, so a frame orders coincident draws the same way every run — a
prerequisite for treating "byte-identical submit counters" as evidence of anything.

## The two design calls worth knowing

**`RenderItem` carries its own bounds.** Culling used to reach through
`mesh->boundsCenter()` per item, per view — and shadow cascades multiply the view
count. Six floats are copied at extraction (`render/renderer.cpp`) instead, which
makes the cull loop a linear scan over contiguous PODs *and* is what removes the
`Mesh` dependency that would otherwise drag bgfx in here.

**Opaque draws sort by material before depth.** Blend class always leads — opaque
before transparent is correctness, not tuning. Below that, opaque keeps material
above depth because the pipeline this replaced sorted by `(meshKey, matKey)`,
pure batching, and flipping to depth-first would trade measured batching away for
early-Z on the strength of a guess. Depth still rides along as the tiebreaker.
Transparent inverts the layout — depth dominates, far first — because anything
else blends wrong. Revisit when a profile says overdraw costs more than state
changes.

## Known limitations
- **`BlendClass` is always `Opaque` today.** `RenderItem` does not carry a blend
  class yet, because the pipeline has no transparent path wired. The sort key
  handles transparency correctly; nothing produces it. Stated rather than hidden.
- **`kMaxLights = 16`, hard.** Lights past the cap are dropped. The fix is
  clustered forward (`docs/architecture/renderer-architecture.md` §2), not a bigger array.
- **No instancing yet.** `batchRunLength()` exists and is tested; `ForwardPipeline`
  does not yet collapse a run into one instanced submit.
- **Single-threaded.** `buildVisibleSet` is a serial scan. The data layout is
  deliberately parallel-friendly; nothing splits it across jobs yet.

## Tier evidence (`working`)
- Builds clean, wired into `engine_runtime`, consumed by `ForwardPipeline`.
- `tests/render_world_test.cpp` — 42 assertions across all four modules, run in
  the `unit` lane. Mutation-proved: inverting the opaque key layout and culling
  unbounded items each produce failures (9 total), reverting restores PASS.
- Live-verified in `fps_shooter`: textures resident, shadows drawn, gameplay
  raycasts hitting.

An external audit of this directory was triaged on 2026-08-04 — see `issues.md`.
Four of ten claims were real (an under-estimating bounding sphere under nested
scale+rotation, Euclidean distance where the key documents view depth, silently
truncating key fields, a fixed parallel grain), four were wrong, and two restated
known gaps. Two of the refutations were only *provable* after writing the tests that
should have existed: `extractFrustumPlanes` had no end-to-end coverage (every frustum
test built planes by hand), and the 12-byte-packing proposal needed measuring rather
than arguing. Both are now in `render_world_test`.

Reaching `hardened` needs a fuzz or stress lane — adversarial frustums
(degenerate planes, NaN transforms) and a many-thousand-item visibility soak.
