---
status: target
verified: 2026-08-01
covers:
  - src/render/
---
# Renderer Architecture (target)

> **This is the design we are building toward, not what exists.** For what the
> renderer *is* today — 9 ranked findings, 3 critical, with measurements — see
> **`docs/plans/renderer-audit-and-plan.md`**. This document supersedes the June 2026
> version of this file, whose migration plan was never executed.

## 1. What this renderer is for

Four requirements, in priority order. Every decision below is traceable to one
of them, and when they conflict the earlier one wins.

1. **Small-to-medium studios ship real games on it.** Good defaults matter more
   than maximum capability: it must look right with nobody touching it.
2. **Art direction is the studio's, not the engine's.** Cel-shaded, stylised,
   or photoreal must all be reachable **without forking the engine**. This is
   the requirement most likely to be lost, because it costs architecture rather
   than features.
3. **The low-end floor is real hardware.** Intel UHD 630, ~128 MB usable VRAM,
   4 GB system RAM. Not a stretch goal — the acceptance test.
4. **Complexity must pay for itself.** One developer maintains this. A
   technique that needs constant care to stay correct is a liability regardless
   of what it renders.

## 2. The decision that shapes everything: clustered forward, not deferred

Deferred shading is the reflex answer for many lights. It is the wrong answer
here, and requirement 3 decides it:

| | deferred | clustered forward |
|---|---|---|
| G-buffer at 1280×720 | ~4 targets ≈ **28 MB**, before shadows | **none** |
| bandwidth per frame | write G-buffer, read it all back | one pass |
| transparency | needs a second forward path anyway | native |
| MSAA | expensive/awkward | works |
| VRAM budget impact | ~25% of the entire 128 MB | ~0 |

On an integrated GPU sharing system memory, **bandwidth is the binding
constraint, not shading math.** A G-buffer spends the scarcest resource first.
Clustered forward keeps one geometry pass, adds a compute/CPU light-binning
step, and scales to hundreds of lights without a fat intermediate.

Today's renderer is *plain* forward with a fixed `MAX_LIGHTS` uniform array —
clustered forward is the natural next step from it, not a rewrite.

**Consequence to accept:** heavy overdraw hurts more than it would in deferred.
Mitigation is a depth prepass (cheap, optional per project) and good culling —
both of which we want anyway.

## 3. Where customization lives — the crux of requirement 2

"Customizable" fails when the extension point is at the wrong granularity.
Today the *only* seam is `IRenderPipeline`: swap it and you inherit nothing —
no culling, no sorting, no light handling. That is a facade, because nobody
reimplements culling to get an outline effect.

Three tiers instead, from most to least used:

| tier | you change | you keep | reaches |
|---|---|---|---|
| **1. Material + shader** | the BRDF, inputs, blend state | everything else | ~90% of art direction |
| **2. Pass graph** | which passes run, in what order | extraction, visibility, submission | outlines, custom post, feature toggles |
| **3. Pipeline replacement** | everything after `RenderWorld` | extraction + visibility | research, exotic pipelines |

**Tier 1 is where cel-shading and photorealism actually differ.** Both need
culling, sorting, batching, shadows, and post — they differ in the shading
function and the post chain. So the material/shader system is the primary
art-direction surface, and it is exactly what does not exist today: `Material`
is a fixed struct of `baseColor/normal/roughness/metallic`, and shaders are
compile-time C arrays baked into the binary (audit R3).

## 4. Frame architecture

```
 ECS world
    │  EXTRACT            jobified; reads components, writes POD
    ▼
 RenderWorld              frozen frame description — no ECS, no game pointers
    │  VISIBILITY         frustum cull → LOD select → bucket + sort keys
    ▼
 VisibleSet               per-view draw lists, sorted, instance-grouped
    │  RENDER GRAPH       passes declare reads/writes; graph orders + aliases
    ▼
 Pass execution           records draws against the backend
    │  SUBMIT
    ▼
 bgfx
```

Each arrow is a hard boundary: the stage on the right never reaches back left.
That is what makes extraction jobifiable, visibility testable headlessly, and
passes swappable.

| stage | input | output | thread |
|---|---|---|---|
| Extract | ECS world | `RenderWorld` | job pool, parallel |
| Visibility | `RenderWorld` + camera | `VisibleSet` | job pool, parallel per view |
| Graph | `VisibleSet` + pass list | execution order + resources | main |
| Submit | `VisibleSet` | backend calls | main (bgfx is single-threaded here) |

## 5. RenderWorld — the contract

A **frozen, POD description of one frame**. No `flecs::entity`, no registry
lookups, no pointers into game state.

```
RenderWorld {
    Span<RenderItem>  items;      // transform, mesh, material, bounds, flags
    Span<LightItem>   lights;
    Span<CameraItem>  views;      // main, shadow cascades, reflections
    FrameConstants    frame;      // time, exposure, ambient…
}
```

Why it is worth the copy:

- **Extraction parallelises** — items are independent, output is a flat array.
- **Passes cannot cheat.** A pass that can reach the ECS eventually will, and
  then it cannot be reordered or reused. This is the boundary that keeps tier-2
  customization honest.
- **Testable without a GPU.** Culling, LOD and sorting become pure functions
  over PODs — which matters because `src/render` has no GPU test harness and
  is the least-verified subsystem in the engine.
- **Determinism.** Same `RenderWorld` → same draws, so golden-image tests and
  frame replay become possible.

## 6. Visibility: culling, LOD, sorting

**Culling, staged by need:**

1. **Frustum vs bounds** — exists today, keep. O(n), trivially jobified.
2. **Hierarchical (BVH over static geometry)** — when n gets large enough that
   O(n) per view hurts. Shadow cascades multiply view count, so this arrives
   sooner than intuition suggests.
3. **Occlusion** — deferred (§10). Needs a real occluder-heavy scene to justify.

**LOD** by projected screen-space error, not raw distance: distance alone
misbehaves with wildly different object sizes and with FOV changes. Meshes
carry an LOD chain from the cooker; selection happens in visibility, so it is
one integer per item and costs nothing at submit time.

**Sort keys — one packed 64-bit integer per draw:**

```
[ viewport 4 | pass 4 | translucency 2 | depth 24 | material 16 | mesh 14 ]
```

Opaque sorts front-to-back within material (early-Z wins, minimal state
change); transparent sorts back-to-front by depth (correctness first). A single
integer compare replaces today's comparator lambda over two fields, and the
same key drives instance grouping: adjacent draws sharing mesh+material batch
automatically.

## 7. Render graph

Passes declare what they **read** and **write**; the graph does the rest.

```
ShadowPass    writes: shadowmap
DepthPrepass  writes: depth
LightBinning  reads: depth        writes: clusters
OpaquePass    reads: depth, shadowmap, clusters   writes: sceneColor
SkyPass       reads: depth        writes: sceneColor
TransparentPass reads: depth, clusters            writes: sceneColor
PostChain     reads: sceneColor   writes: backbuffer
```

Two things fall out of that declaration, and both are load-bearing:

**Customization (tier 2).** Adding a cel-shading outline pass is: declare it,
say it reads depth+normals and writes sceneColor, insert it. No engine fork, no
reimplementing culling. Removing bloom on the low-end target is deleting a node.

**Memory.** The graph knows each transient resource's lifetime, so buffers
whose lifetimes do not overlap **alias the same allocation**. On a 128 MB
budget this is not an optimisation, it is the difference between a post chain
fitting and not. We already measured the cost of *not* managing this: a single
hardcoded 4096² shadow map was 64 MB — 90% of a shipped frame's GPU memory —
until it became a project setting.

Start with a **static graph** (fixed node list, dependencies checked at build)
rather than a fully dynamic one. It gets aliasing and composition; it skips the
complexity that only pays with dozens of optional passes.

## 8. Materials and shaders — the art-direction system

The heart of requirement 2, and the biggest single change from today.

**Material becomes data, not a struct:**

```
material {
  shader: "shaders/cel.shader"     // which shading model
  params: { rampTex, outlineWidth: 2.0, baseColor: [...] }
  state:  { blend: opaque, cull: back, depth: less }
}
```

Not `roughness`/`metallic` fields — those are *PBR's* parameters, and hardcoding
them means the engine has already chosen photorealism. A cel material has a ramp
texture and a step threshold; a PBR material has roughness and metallic; the
engine should not care which.

**Shaders become cooked assets.** `.sc` sources go through a `ShaderCooker` into
the existing cook pipeline, so shader binaries get DDC caching, per-platform
variants, and content-addressed sharing exactly like meshes and textures — one
compile per variant per studio, not per developer. This also kills the current
situation where adding a material type requires rebuilding the engine.

**Permutations are the risk.** Variants multiply (skinned × shadows × fog ×
lightmapped …) and naive expansion explodes. Rules: permute only on things that
must be compile-time, prefer uniform branches for the rest, and cook variants
**on demand** — the DDC makes an uncooked variant a first-use cost, not a
release-build cost.

**Shading model is a shader-side function**, so a project supplies its own:

```
// cel.shader
vec3 shade(SurfaceData s, LightData l) {
    float ndl  = dot(s.normal, l.dir);
    float band = texture(u_ramp, vec2(ndl * 0.5 + 0.5, 0)).r;
    return s.albedo * l.color * band;
}
```

The engine owns the loop over lights, the cluster lookup, the shadow test —
the project owns the pixel's colour. That split is what lets one renderer serve
both looks without either being a special case.

## 9. Efficiency ledger

What each technique buys, and when to build it. Measured numbers are from
fps_shooter on this hardware; unmeasured ones are marked.

| technique | buys | build when |
|---|---|---|
| GPU resource cache | dedup + eviction; makes leaks *definable* | **done** (Phase 1) |
| Shadow resolution as data | **71 MB → 23 MB shipped VRAM (measured)** | **done** |
| Cooked BC textures + mips | **76 MB → ~0 MB (measured)** | **done** |
| Render-target aliasing | the post chain fits in budget | with the graph |
| Instancing | draw calls ÷ N for repeated meshes | when draws > ~500 (now: **13**) |
| Sort-key packing | fewer state changes, cheaper sort | with the graph |
| Depth prepass | kills overdraw shading cost | when overdraw measurably hurts |
| Hierarchical culling | CPU cull cost at high object counts | when cull time shows in the profile |
| LOD | vertex + fill cost at distance | when scenes have distance |
| Occlusion culling | indoor/dense scenes | §10 — needs a scene that proves it |

**Note the draw count: 13.** Instancing is a correct future investment and a
useless present one. The ledger exists so we build in the order the profile
dictates, not the order the literature suggests.

## 10. Deliberately not building (and the trigger that changes it)

- **Deferred / visibility buffer** — trigger: the low-end target stops being a
  requirement. (§2.)
- **Occlusion culling** — trigger: a real scene where frustum culling leaves
  most draws invisible.
- **Virtual textures / streaming megatextures** — trigger: texture working set
  exceeds VRAM after quality tiers. Not close.
- **Real-time GI, ray tracing** — trigger: hardware floor rises well above a
  UHD 630. Baked lighting first; it is what a small studio ships anyway.
- **Mesh shaders / bindless** — trigger: bgfx exposes them portably.
- **Multi-threaded submission** — trigger: submit shows up in the profile
  (today: 0.13 ms).

Each is defensible to revisit *with a measurement*. None is free, and every one
of them costs more maintenance than it looks like it will.

## 11. Migration

Ordered so each step leaves a working renderer and delivers something on its
own. Detail and current status live in `docs/plans/renderer-audit-and-plan.md`.

1. **GPU resource cache** — identity, refcounts, budget. *Done.*
2. **Render tooling** — VRAM census, duplicate report, leak detector. Turns
   `src/render` from untestable into measurable.
3. **Extraction split** — build the real `RenderWorld`; move culling, sorting
   and light packing out of the pipeline. Unlocks tiers 2 and 3 for real.
4. **Render graph** — static node list, declared resources, aliasing.
5. **Material + shader assets** — `ShaderCooker`, data-driven materials,
   pluggable shading model. This is when requirement 2 is actually met.
6. **Scale work** — instancing, LOD, clustered lights, hierarchical culling,
   each behind its trigger in §9.

Steps 1–2 are foundation, 3–4 are architecture, 5 is the product promise, 6 is
performance. Doing 6 before 3 is the classic trap: optimisations wired into a
monolith have to be rewritten when the monolith is split.
