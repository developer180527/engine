---
status: target
verified: 2026-08-01
covers:
  - src/render/
---
# Renderer Architecture

> **Superseded in part (2026-08-01).** The GOALS below still stand. The
> migration plan does not: an audit found that the extraction/RenderWorld split
> described here was never built — culling, sorting and light packing still
> live inside `ForwardPipeline`, and `src/render/passes/` remains scaffold that
> is not in the build. See **`docs/renderer-audit-and-plan.md`** for what the
> renderer actually is today, the measured budgets, and the sequenced plan that
> replaces this document's migration section.

> **Status:** This documents the *target* architecture for the default engine
> renderer and the path to it. Parts already exist (the pipeline seam, the
> extraction in `Renderer`, the `RenderView`/`RenderContext` packets); the
> per-pass decomposition under `src/render/passes/` is **scaffold** — commented
> stubs, not yet in the build. We migrate to it incrementally, feature by
> feature, never as one big rewrite.

## Goals

- **Separate *what to draw* from *how to draw it*.** Gameplay/ECS decides what
  exists; the renderer decides how it appears. Neither reaches into the other.
- **Data-oriented.** Each frame the ECS world is *extracted* into flat,
  GPU-ready data. Passes consume that and never touch flecs, registries, or
  component types directly.
- **Composable passes.** A frame is an ordered list of small, single-purpose
  passes instead of one monolithic function.
- **Pluggable technique, not pluggable backend.** Swap the whole pipeline
  (`IRenderPipeline`) or reorder/replace passes. The backend stays bgfx/Metal —
  no speculative RHI abstraction (YAGNI).
- **Incremental & shippable.** Every migration step leaves a working engine.

## Data flow
ECS world (flecs)
|  extraction  (Renderer::buildView runs the cached queries)
v
RenderWorld  -- flat, GPU-ready, backend-agnostic --+   + per view:
- opaque items (mesh, material, world matrix)     |     camera, frustum,
- transparent items (sorted)                      |     target, view ids
- lights (packed)                                 |
|                                             v
Pipeline (IRenderPipeline) -- ordered list of passes --
setup(fc)   x all passes   (producers publish resources)
execute(fc) x all passes   (consumers read them, submit draws)
|
v
bgfx views -> Metal command buffers -> screen

## The pieces

### Existing today
- **`RenderView`** (`render/render_view.h`) — one camera's frame packet: view/
  proj, `camPos`, the 6 frustum planes, spans of `RenderItem` + `LightItem`,
  ambient, the `RenderTarget`, and the base view id.
- **`RenderContext`** (`render/render_context.h`) — engine-side handles a pass
  needs: asset/texture/material registries, fallback white + flat-normal
  textures, the `viewCursor` allocator, and the reserved `shadowViewId`.
- **`IRenderPipeline`** (`render/render_pipeline.h`) — the swappable seam:
  `onAttach` / `onDetach` / `render(view, ctx)`. Injected via
  `Renderer::setPipeline()`; default is `ForwardPipeline`.
- **`Renderer`** (`engine/renderer.h`) — owns the bgfx device, framebuffers,
  fallback textures, reserved view ids, the cached extraction queries, and runs
  extraction (`buildView`) before handing the packet to the pipeline.
- **`ForwardPipeline`** (`render/forward_pipeline.h`) — the *current* monolith:
  does the shadow depth pass AND the main lit forward submit in one class. The
  passes below decompose it.

### New / planned
- **`RenderWorld`** (`render/render_world.h`, *TODO*) — the extraction output:
  opaque + transparent buckets (culled, sorted) and packed lights, fully
  resolved (mesh/material/texture looked up once, not per-draw). Today it lives
  implicitly as the `RenderItem`/`LightItem` vectors inside `Renderer`;
  promoting it to a named type is what lets multiple passes share one extraction.
- **`IRenderPass`** (`passes/i_render_pass.h`) — the per-pass contract.
- **`PassContext`** (`passes/pass_context.h`) — per-frame blackboard + the
  explicit inter-pass hand-off slots.
- **`PassListPipeline`** (`passes/pass_list_pipeline.h`) — an `IRenderPipeline`
  whose body is an ordered `vector<IRenderPass>`; the migration target.

### The passes
| Pass | View | Status | Does |
|------|------|--------|------|
| `ShadowPass` | shadow (0) | in ForwardPipeline | depth from the sun -> shadow map + matrix |
| `SkyPass` | bg (2) | a clear today | background / skybox |
| `OpaquePass` | scene (1) | in ForwardPipeline | lit forward submit of opaque geometry |
| `TransparencyPass` | scene (1) | **not built** | sorted alpha-blended geometry |
| `PostPass` | — | **not built** | tonemap / bloom; color-space fix lands here |
| `ResolvePass` | resolve (3) | a blit today | MSAA resolve -> final target |

## View-id scheme

bgfx submits views in ascending id order, so ids encode frame ordering:
0  shadow    depth-from-light (must precede the scene that samples it)
1  scene     main scene color
2  bg        background / sky
3  resolve   MSAA resolve / present
4  game      game-camera view (Play)
5+ dynamic   allocated via RenderContext::viewCursor

Reserved ids live on `Renderer`; passes get the ones they need through
`PassContext`/`RenderContext` rather than hardcoding them.

## Migration plan

Each step is independently shippable and pixel-identical:

1. **Land the contracts** — `IRenderPass` + `PassContext` (this scaffold).  [done]
2. **`RenderWorld`** — promote the extraction output to a named shared type.
   *Trigger: a second pass needs the same extracted data.*
3. **`ShadowPass`** — move `ForwardPipeline::renderShadow` here.
   *Trigger: CSM (cascades are N shadow sub-passes — when the pass earns its keep).*
4. **`OpaquePass` + `SkyPass`** — move the main submit + the background clear.
5. **`PassListPipeline`** — compose the passes; switch `Renderer`'s default to it.
6. **Delete `ForwardPipeline`** once parity is verified.
7. **`TransparencyPass` / `PostPass`** — when those features are actually needed.

## Deferred (with triggers) — what we are NOT building yet

- **Frame graph** (auto ordering from declared resource reads/writes). The flat
  ordered list + explicit `PassContext` hand-offs are clearer and cheaper.
  *Trigger: hand-offs grow data dependencies we can't keep correct by hand.*
- **RHI / backend abstraction.** Single backend (bgfx/Metal).
  *Trigger: a real need for a second backend — not anticipated.*
- **Forward+ / clustered lighting.** `MAX_LIGHTS = 16` forward loop is fine;
  excess visible lights drop silently (sort by influence to mitigate).
  *Trigger: scenes with hundreds of lights.*
- **Cascaded Shadow Maps.** Single fitted directional map today.
  *Trigger: need for crisp-up-close AND long-range shadows (also the trigger for step 3).*

## Color space

Lighting currently accumulates in **gamma space** (a deliberate, zero-regression
choice). The correct fix — work in **linear**, convert to sRGB once — belongs in
`PostPass` alongside tonemapping, on an HDR (RGBA16F) scene target. Until PostPass
exists, do not scatter half-conversions through the shaders.

## File map
src/render/
render_view.h         RenderView, RenderItem, LightItem, RenderTarget   (exists)
render_context.h      RenderContext                                     (exists)
render_pipeline.h     IRenderPipeline                                   (exists)
forward_pipeline.h    ForwardPipeline - current monolith                (exists)
render_world.h        RenderWorld - extraction output                   (TODO)
passes/
i_render_pass.h        IRenderPass contract                  (scaffold)
pass_context.h         per-frame blackboard + hand-offs      (scaffold)
shadow_pass.h          directional shadow (-> CSM)           (scaffold)
sky_pass.h             background / skybox                   (scaffold)
opaque_pass.h          lit forward opaque submit             (scaffold)
transparency_pass.h    sorted alpha (future)                 (scaffold)
post_pass.h            tonemap / color space (future)        (scaffold)
resolve_pass.h         MSAA resolve / present                (scaffold)
pass_list_pipeline.h   IRenderPipeline as an ordered list    (scaffold)
