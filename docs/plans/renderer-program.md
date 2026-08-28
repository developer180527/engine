---
status: plan
covers:
  - src/render/
---
# The renderer programme — one AAA renderer, on a reusable substrate

> **Status: plan. Nothing here is built.** No `verified:` date, because there is
> nothing as-built to verify. This document decides *what we are doing and in what
> order*; `docs/plans/rhi-design.md` holds the GPU-driven design itself and stays
> the technical reference for Phases 2–8.

## 0. The decision, in one paragraph

We are building **one renderer, designed for a AAA game engine, iterated for about
a year toward AAA quality and performance** — and we are drawing the reuse boundary
*below* it, not through it. The graphics backend and the GPU-driven substrate are
built to be reused by other 3D applications. The renderer on top of them is ours,
opinionated, and free to stay that way. Developers get control through the **render
graph** and the **material/shader system**, not by replacing the renderer.

## 1. Why the boundary is below the renderer, and not through it

The tempting version of this project is "a renderer that serves a game engine and a
CAD application equally." That is the version that explodes, and the reason is a
single rule:

> **A shared component stays simple when it has one job and no opinion about the
> domain. It explodes when it must satisfy conflicting requirements from different
> domains.**

Applied honestly, layer by layer:

| layer | its job | game vs CAD vs a texturing tool | verdict |
|---|---|---|---|
| **RHI** | talk to the GPU | identical | **share** |
| **Substrate** — persistent instances, compute cull, indirect draw, bindless, render graph | draw N things fast | identical; all three want exactly this | **share** |
| Materials / shading model | look right | PBR vs flat + section caps vs live preview | **do not share** |
| Lighting, shadows | — | cascades vs studio three-point vs none | **do not share** |
| Passes | — | opaque/transparent vs edges/picking/section | **do not share** |
| Post chain | — | TAA, bloom vs crisp AA vs none | **do not share** |

The word "renderer" is what does the damage: it bundles a domain-free substrate
with the most opinionated code in a 3D application.

**The industry evidence points the same way.** bgfx is used by thousands of
unrelated products — including, today, both this engine and vCAD. NVRHI is reused
by RTXDI, RTXGI and Donut, and it deliberately does not compile or reflect
shaders. AMD's RPS is a render graph extracted as a standalone product, and it has
been quiet since 1.1 in 2023 — a middle layer with no product on top does not
sustain. Filament is an excellent *renderer* and is rarely used outside its
intended domain, because its opinions about materials and lighting cannot be
removed.

> **RHIs and substrates get reused across domains. Renderers get forked.**

So: share the substrate, keep the renderer, and let a second consumer bring its
own thin renderer. Adding CAD edge rendering never touches the game path; adding
skinning never touches CAD. The "justify every feature for two consumers" tax that
would otherwise be charged on every future feature is not charged at all, because
the shared layer has no domain features to argue about.

### 1.1 What this costs, stated once

Two consumers is still a real constraint on the shared layer, and it lands in
exactly one place: **Metal is a shipping backend, not a dev backend.** vCAD ships
on macOS and iPad, where Metal is the only backend that exists. `rhi-design.md`
axiom 6 said the opposite until 2026-08-28 and has been corrected.

The backends are **Metal 4 and Vulkan 1.3, and there is no third.** Between them
they reach macOS, iPadOS, iOS, Windows, Linux, Steam Deck and Android — every
platform either product ships on. D3D12 adds exactly one that neither covers,
Xbox, and that is its trigger.

Cutting from three backends to two is the single largest complexity reduction
available in this programme, and it costs nothing shipped today. `rhi-design.md`
§9 names the dominant unschedulable risk as the driver quirks bgfx absorbs for
free, rediscovered one vendor at a time; a third backend multiplies that, plus its
validation setup and its CI leg, forever, for one developer.

**Metal 4 is what makes two sufficient rather than a compromise.** Metal 3 tracked
resources implicitly and was genuinely a different shape. Metal 4 is explicit —
untracked resources with explicit barriers, `MTL4ArgumentTable` instead of
per-resource binding, residency sets, reusable command buffers — which is
Vulkan's shape. Enough divergence remains (residency sets vs memory heaps,
argument tables vs descriptor sets, the queue model) that satisfying both keeps
the abstraction honest, which is what leaves D3D12 a later *backend* rather than a
later *redesign*.

## 2. Where developers get control

Three tiers, from `renderer-architecture.md` §3, with the emphasis this programme
puts on each:

| tier | you change | you keep | this programme |
|---|---|---|---|
| **1. Material + shader** | shading model, inputs, blend state | everything else | **Phase 5** — the art-direction surface |
| **2. Render graph** | which passes run, in what order | extraction, visibility, submission | **Phase 4** — the primary control surface |
| **3. Pipeline replacement** | everything after the scene | scene + visibility | exists, kept, rarely the answer |

Tier 2 is the one this document promises. A render graph whose passes declare
their reads and writes is what makes "add an outline pass", "drop bloom on the low
spec", and "insert an ID pass for picking" additive rather than a fork. It is also
what derives barriers and aliases transient targets, which on a 128 MB VRAM floor
is not an optimisation but the difference between a post chain fitting and not.

## 3. The two facts that set the order

**Fact one: extraction is the bottleneck, not submission.** CPU 24.8 ms against GPU
9.11 ms at 50 000 real props (`issues.md` R20). An RHI rewrite does not touch that.
The fix is to stop rebuilding the frame every frame — a **retained scene** the host
mutates with deltas, which is also exactly what a persistent GPU instance buffer
is. One mechanism, both problems.

**Fact two: we have never tried GPU-driven rendering on the backend we already
have.** bgfx has compute dispatch, `createIndirectBuffer`, storage buffers and
`submit(view, program, indirectHandle)`. This engine has **zero call sites** for
any of them. What bgfx genuinely lacks is *bindless*, and bindless is what blocks a
GPU-driven path over a scene with **per-object textures** — not one whose per-object
variation is an index into a parameter buffer.

So the first move is a spike, not a backend.

## 4. Phases

Each must be independently defensible. Phases 0a–3 need no GPU farm and no new
backend; that is deliberate, because `rhi-design.md` §3 was gating structural work
behind a measurement rig it does not require (corrected in §3.1 there).

| # | Phase | Exit criterion |
|---|---|---|
| **0a** | **The bgfx GPU-driven spike.** Compute cull → compacted indirect args → indirect draw, per-instance material index from a storage buffer, on the 50 k fuzz scene. | Extraction stops dominating, **or** we can name the exact wall in one sentence. Weeks. **This decides whether Phases 5–8 are worth a year.** |
| **1** | **De-contaminate.** The **five** files that mix loading with GPU upload (`asset_service.cpp`, `async_loader/upload.cpp`, `mesh_loader.cpp`, `gltf_importer.cpp`, `assimp_importer.cpp`); `Renderer` behind a pointer in `runtime.h`. | `engine_runtime` links for a server target with no graphics libs; tests green. |
| **2** | **Close the seam.** Opaque handles; `RenderContext`/`RenderView` stop naming `bgfx::TextureHandle`, `FrameBufferHandle`, `ViewId`. Stable `objectId` on `RenderItem`. | A second backend is *expressible*. `include/engine/render.h` exposes no bgfx type. |
| **3** | **Retained scene.** Concretely, three things and no more (§8.2): a **heap sub-allocator** over one instance buffer, a **sparse uploader** that pushes only dirty ranges, and **one apply point per frame** with a hard ownership rule. Host mutates via create/destroy/setTransform/setVisible. | The 24.8 ms extraction number moves, measured on the fuzz scene. |
| **4** | **Render graph.** Passes declare reads and writes; barriers derived, transient targets aliased. Topology **cached and invalidated on change**, not rebuilt per frame (§8.3). | Shadow + opaque + one post pass through the graph; peak VRAM measurably below the hand-managed version. **Tier-2 control ships here.** The bgfx/RHI coexistence seam has a defined barrier contract (§8.4). |
| **5** | **Material + shader assets.** Data-driven materials, pluggable shading model, cooked shader variants through the DDC. | A project supplies a cel shading model without forking the engine. **Tier-1 control ships here.** |
| **6** | **GPU measurement lane** — one NVIDIA + one AMD box on the farm. | bgfx's current numbers reproduced on discrete hardware. Gates everything after it. |
| **7** | **`rhi` core** — device, queues, timelines, resources, bindless, command lists. **Metal 4 + Vulkan 1.3, designed together. No third backend.** | One triangle on both, under validation layers, zero warnings. |
| **8** | **Port the forward pipeline, no per-draw uniforms**, then GPU-driven, then RT / mesh shaders. | As `rhi-design.md` §8 Phases 4–8. |

Phases 5–8 are `rhi-design.md`'s project, unchanged except for backend priority.
Phases 0a–4 are this document's, and every one of them is worth doing even if the
RHI is never built.

## 5. The costs, all of them

| cost | size |
|---|---|
| Phases 0a–4 | weeks each; the whole group is months, not a year |
| Phases 6–8 (`rhi-design.md` §9) | ~6 months to beat what exists, ~1 year to the full vision |
| **Retained-mode lifetime bugs** | permanent, a new bug class immediate mode cannot have |
| **Testability regression** | `rworld::` is GPU-free pure functions with headless tests today. GPU-driven moves cull and draw counts onto the GPU, where `SubmitStats` cannot see them. `rhi-design.md` §6 has the mitigation: the CPU path is the readback oracle, not vestigial code |
| **Driver quirks** | the dominant unschedulable risk; bgfx absorbs ~15 years of them for free |
| **Two backends** | Metal 4 and Vulkan 1.3. Metal has a shipping product on it, so it cannot be the neglected one. D3D12 is deferred; Xbox is its trigger |
| **The low-end floor is unresolved** | `renderer-architecture.md` makes Intel UHD 630 the acceptance test; `rhi-design.md` §11.3 now records the tiered spec that reconciles it — GPU-driven reaches that floor, mesh shaders and RT do not and must be optional tiers |
| **No golden-image testing** | still the reason `src/render` is `tier: working`; Phase 6 is what finally makes it possible |

## 6. What would make us stop

- **Phase 0a moves nothing and names no wall.** Then the bottleneck is not where
  this document says, and the answer is incremental extraction and job-system work.
- **Phase 3 lands and extraction is still the frame's cost.** Then the retained
  scene was the wrong mechanism and Phases 7–8 are unjustified.
- **Phase 6 shows the GPU is nowhere near the limit on discrete hardware.**
  `rhi-design.md` §10 already says this and it still holds.
- **The shader toolchain slips past ~6 weeks** — the classic sinkhole.

## 7. Open decisions

1. **Does the RHI compile shaders?** Recommend **no** — it takes bytes, the cooker
   stays host-side. NVRHI's choice, and it is what stops a second consumer
   inheriting our content pipeline.
2. **Bindless-only, or binding sets?** Recommend keeping `rhi-design.md` axiom 2
   (bindless-only) — GPU-driven at 50 k objects needs it — while recording that
   every reusable RHI shipping today chose binding sets, explicitly for validation.
3. **The three floors, written down as tiers.** `rhi-design.md` §11.3 now carries
   the desktop reconciliation (UHD 630 survives GPU-driven; mesh shaders and RT
   become optional tiers). The **iPad floor is still unstated** — argument buffers
   and `MTLIndirectCommandBuffer` set it — and it has to be a number before Phase 7.
4. **Does vCAD adopt the substrate, or stay on bgfx?** It can stay on bgfx through
   Phase 0a and adopt later. Nothing in this plan requires deciding now, and that
   is intentional.

## 8. Prior art, and what we take from it

Written because the alternative is rediscovering, one bug at a time, things senior
engineers have already published. Each subsection is a mechanism someone shipped,
what it cost them, and the one line we are taking from it.

**A note on sourcing.** Everything below is from material actually read. Decima's
visibility talk is the gap: the Guerrilla page links a PDF and PPT that could not
be retrieved, so all that is claimed here is what the abstract states — PS4
async-compute visibility queries for the open world, and "efficiently collecting
batches of object instances." **Nothing else about Decima should be inferred from
this document until someone has read the slides.** A half-remembered talk is worse
than an admitted gap, because the next reader cannot tell which is which.

### 8.1 What "retained" does and does not mean

Three different things get called "the scene," and only one of them is in dispute.
Naming them separately removes most of the confusion this decision attracts:

| | what it is | today | after Phase 3 |
|---|---|---|---|
| **Resources** — mesh vertex/index buffers, textures | GPU objects | **already retained** | unchanged |
| **Scene description** — which objects exist, their transform, mesh and material | array of ~50 000 records | **rebuilt from scratch every frame** | **retained, patched** |
| **Visible list** — which of them are on screen this frame | a subset, sorted | rebuilt every frame | rebuilt every frame, on the **GPU** |

Row 1 was never the question — nobody re-uploads meshes per frame. Row 3 is
rebuilt every frame in every engine ever written; GPU-driven only changes *where*.
**Only row 2 changes**, and the argument for it is one sentence:

> Between two frames maybe 200 of 50 000 objects changed. We rebuild all 50 000 to
> express 200 changes. That is the measured 24.8 ms.

Also worth stating because the term carries baggage: this is **not** 1990s retained
mode. Direct3D Retained Mode and the scene-graph libraries of that era were
opinionated hierarchies — nodes, traversal, virtual dispatch — and deserved to die.
What Unreal and Unity do is a **flat SoA table with stable integer handles**: no
hierarchy, no traversal, no polymorphism. `RenderItem` is already close to the
right shape; what changes is who owns it and how long it lives.

### 8.2 Unreal and Unity — the sync, which is the hard part

**Unreal: the ownership rule and the apply point.** Two objects with two lifetimes
— `UPrimitiveComponent` on the game thread, `FPrimitiveSceneProxy` on the render
thread — and an absolute rule: *the render thread never touches component memory,
and the game thread never touches proxy memory after registration.* All traffic
goes through `ENQUEUE_RENDER_COMMAND`. The game thread **blocks at the end of each
Tick** until the render thread is within one to two frames. Crucially, updates are
not applied as they arrive: they are batched and integrated at a single point, in
`FScene::UpdateAllPrimitiveSceneInfos()`.

> **Take:** the sync is a *batched command queue applied at exactly one point in
> the frame*, under a hard ownership rule — not per-object messages crossing a
> boundary whenever something moves. Copying Unreal's proxy classes would be
> cargo-culting; copying the ownership rule and the single apply point is the
> actual lesson, and it is the part they spent years hardening.

**Unity: the two data structures, by name.** Instance data lives in a GPU
`ByteAddressBuffer`; the shader receives a **single 32-bit metadata value** telling
it where to read. The CPU uploads only what changed. Two named helpers make it
work: a **`HeapAllocator`** that sub-allocates the buffer, and a
**`SparseUploader`** that pushes sparse dirty ranges.

> **Take:** Phase 3 is not "design a retained scene." It is *a sub-allocator, a
> sparse uploader, and an apply point.* That is a far smaller and more testable
> thing than the phrase suggests, and it is why Phase 3 sits before any RHI work.

**The risk both of them carry.** The bookkeeping is not what bites — **change
detection** is. Miss an update and an object renders at a stale transform, which
looks like a physics bug and is not. Mark everything dirty and the complexity is
paid for nothing. In this tree that means flecs `OnSet` hooks and observers, and it
is the part of Phase 3 to budget most time for. Unreal's game-thread/render-thread
sync is famously among the more bug-prone areas of that engine, and it is the same
problem.

### 8.3 Render graphs — production settings for the dials

UE5's RDG and Frostbite's FrameGraph both run **700+ passes**, which is the scale
that makes the graph worth its complexity. Both compile the same way: topological
sort → dead-pass culling → lifetime scan for aliasing → barrier computation with
split barriers → async fences → barrier batching into single API calls.

Where they differ is the useful part, because it is two shipped answers to the
same question:

| | UE5 RDG | Frostbite |
|---|---|---|
| Graph topology | **cached, invalidated on change** | **rebuilt every frame** |
| Pass declaration | macro-generated metadata (`BEGIN_SHADER_PARAMETER_STRUCT`) | lambda-based |
| Async compute | **manual opt-in** (`ERDGPassFlags::AsyncCompute`) | automatic, via reachability analysis |
| Aliasing | transient only; imported resources excluded | resources with fully-known lifetimes |

> **Take:** cache the topology (UE5's answer) — our pass list is small and nearly
> static, so rebuilding a graph every frame is cost with no benefit at our scale.
> Start async compute **manual**: UE5 shipped 700 passes that way, and automatic
> discovery is a second system to get wrong before the first one works.

Also recorded as a cost taken knowingly: UE5's macro-based parameter declarations
"trade debuggability and dynamic composition for compile-time safety." We would be
choosing the other side of that trade by default, and should do so deliberately.

### 8.4 The pitfall aimed directly at us

From the same comparison, and it is the one to design for rather than discover:

> **Where graph-managed code meets non-graph code, barriers must be inserted
> MANUALLY, and external resources explicitly registered** (`RegisterExternalTexture()`
> in UE5).

That is not a hypothetical for this programme — it is the state we will be in for
*months*. Phase 4 introduces the graph while `ForwardPipeline` still runs on bgfx,
and Phases 7–8 add an RHI path while both still exist. **Three renderers coexisting
across two backends is the normal condition of this plan, not an edge case.**

> **Take:** the bgfx/RHI coexistence seam needs a defined barrier and
> resource-registration contract *before* Phase 4 ships, not after the first
> corruption bug. It is an exit criterion on Phase 4 for that reason.

### 8.5 What we still owe this section

- **Decima's visibility talk**, read properly. It is the closest published work to
  our exact problem — a very large object set, async-compute visibility, instance
  batch collection — and it is currently a citation with no content behind it.
- **A GPU-driven reference beyond the vendor docs.** The foundational
  Haar/Aaltonen material on GPU-driven pipelines is the obvious next read.
- **Anything on RAGE.** Rockstar publish little; if there is a credible technical
  account it has not been found yet, and its absence should be stated rather than
  filled with inference.
