---
status: plan
covers:
  - src/render/
---
# The G sequence — phases and effort

*Original `rhi-design.md` §8 and §9. Split out 2026-09-04; wording preserved,
including the `G` ids that other documents cite.*

## 8. Phases, with exit criteria

> **These are the `G` sequence.** Three renderer documents run phase sequences and
> all three used to number from 1 unprefixed, so "Phase 4" meant this document's
> forward-pipeline port, `renderer-program.md`'s render graph, AND
> `renderer-audit-and-plan.md`'s submission-efficiency work, which is **already
> done**. `A` is the audit's, `P` is the programme's, `G` is this directory's; `R`
> is reserved for the audit's finding ids. `renderer-program.md` §4 carries the
> P↔G mapping and is the document that decides ORDER — this directory decides
> content.

Each phase must be independently defensible — no phase justified only by the next.

| # | Phase | Exit criterion |
|---|---|---|
| **G0a** | **The bgfx GPU-driven spike** ([`evidence-bgfx.md`](evidence-bgfx.md) §2.2): compute cull → indirect args → indirect draw, per-instance data from a storage buffer, on the 50 k fuzz scene. No new backend. | Either extraction stops dominating, or we can name the exact wall. **Weeks, not months, and it decides whether G2–G6 are worth a year.** |
| **G0b** | **GPU measurement lane on the farm** (one NVIDIA + one AMD box, D3D12 + Vulkan, GPU timings into the SQLite results DB) | The current bgfx renderer's numbers reproduced on discrete hardware. **We will learn things here that change the rest of this plan.** Gates G4–G8; does NOT gate G1 ([`method-measurement.md`](method-measurement.md) §3.1). |
| **G1a** ✅ | **The asset path.** `render/gpu.h`: opaque handles, an opaque staging blob, five operations. All five non-test, non-editor files that mixed loading with upload are clean — **landed 2026-09-04** | Three checks, each seeing what the others cannot: `scripts/check_gpu_seam.py` (grep, `unit` lane) fails the build if any file outside `src/render/` includes a graphics API; `tests/headless_include_probe.cpp` catches the TRANSITIVE case a grep cannot; `tests/gpu_seam_test.cpp` pins the BEHAVIOUR neither can see — headless produces no handles, and texture formats are checked before staging. All mutation-checked. **65/65 unit, 6/6 fuzz, 2/2 asset, 7/7 stress, 1/1 perf** |
| **G1b** ✅ | **`RenderContext`** now carries `gpu::TextureHandle` and `gpu::ViewId` — **landed 2026-09-04**, alongside G1c and for the same reason: it was the last transitive path from `runtime.h` to bgfx | A third-party `IRenderPipeline` compiles with bgfx absent from the include path, asserted by `tests/headless_include_probe.cpp`. The `ViewId` worry was misplaced: aliasing the concept costs nothing and prejudges nothing — the render graph still deletes it (axiom 4) |
| **G1c** ⚠️ | **Headless: headers done, link NOT done — landed 2026-09-04.** `runtime.h` compiles with bgfx entirely off the include path; `renderer.h`, `render_view.h`, `render_context.h`, `mesh.h`, `texture.h`, `vertex.h`, `asset_registry.h` and `primitive_library.h` are all backend-free | **Header half:** `headless_includes` in the `unit` lane, mutation-checked. **Link half:** `engine_runtime_server` (step B) links with zero bgfx symbols and no graphics frameworks — 0 vs `engine_host`'s 4030, 13 MB vs 29 MB — asserted by `tests/server_link_probe.cpp`, which BOOTS and ticks the engine rather than merely referencing it. Mutation-checked: a bgfx include in any server TU fails to compile |
| **G2** | **`rhi` core**: device, queues, timelines, buffers, textures, bindless heap, command lists. Metal 4 + Vulkan. Triangle. | One triangle, both backends, under validation layers with zero warnings |
| **G3** | **Shader toolchain**: HLSL → DXC → DXIL/SPIR-V through the existing cooker and DDC | Every current shader cooks and renders on both backends |
| **G4** | **Port the forward pipeline 1:1**, CPU-driven, but *no per-draw uniforms* — per-draw data in a structured buffer indexed by draw ID | Pixel-comparable to bgfx on the fuzz scene, **and the 4 096 draw ceiling is gone**. First point where we beat bgfx on something real. |
| **G5** | **Render graph**: declared passes, derived barriers, transient aliasing | Shadow + opaque + a post pass through the graph; peak VRAM measurably lower than the hand-managed version |
| **G6** | **GPU-driven**: compute cull → HZB occlusion → indirect draws; incremental instance upload | `Render.extract` no longer scales with entity count. **This is the phase the whole directory is for.** |
| **G7** | **Inline ray tracing**: BVH management, RT shadows, RTAO, gameplay ray queries | RT shadows replace the shadow map on the fuzz scene at equal or better cost |
| **G8** | **Mesh shaders / cluster LOD** — where decimation graduates toward Nanite-style clusters | Dense geometry stops costing draws at all |

> **The G1c decision, corrected 2026-09-05.** This block previously offered a
> null renderer and conditional compilation as ALTERNATIVES and recommended the
> first. That framing was wrong, and verifying Unreal's actual mechanisms is
> what showed it — see [`headless.md`](headless.md) §3.
>
> `engine_runtime` still links bgfx because `EngineRuntime` calls
> `Renderer::frame()`, `renderScene()` and 15 others unconditionally — guarded
> at runtime by `m_headless`, but compiled in.
>
> **The two mechanisms are sequential, not alternatives, and Unreal ships both:**
>
> * **A — `IRenderer` + `NullRenderer`. LANDED 2026-09-05.** Lets the SAME binary run with no GPU:
>   tests, the cook worker, `engine_host`, CI. Unreal's equivalent is
>   `FNullDynamicRHI`, selected by `-nullrhi`, whose official description is
>   *"Use null rendering hardware interface to run UE headless."* Note it sits at
>   the RHI layer, so the whole renderer still runs and terminates in a backend
>   that does nothing.
> * **B — a build target that excludes the render TUs and bgfx. LANDED.**
>   `engine_runtime_server` produces a lean server binary. Unreal's `TargetType.Server`: *"Same as Game, but does not
>   include any client code."* **This, not A, is what G1c's exit criterion asks
>   for** — under A the renderer still links and still builds draw lists into
>   nothing, which is precisely what Epic does NOT ship servers as.
>
> **A is done** (`src/render/renderer_interface.h`, `renderer_null.h`,
> `tests/null_renderer_test.cpp`). It deleted four of the nine scattered
> `if (!m_headless)` guards — the ones that existed only to avoid calling a dead
> renderer. The other five are platform, semantic, or genuine work-skips and stay.
> It was done on correctness rather than performance: the guards' failure mode is proven — one guard
> present and an adjacent one missing leaked 480 KB/s on a server, silently
> (`src/runtime/docs/issues.md`, 2026-08-10). With pure virtuals a missing
> override is a compile error, so axiom 5's rot hazard does not apply to a
> do-nothing implementation.
>
> **Neither is the biggest server win.** That is cook-time class exclusion —
> Unreal's `UObject::NeedsLoadForServer()` plus `ClassesExcludedOnDedicatedServer`
> — which keeps the assets from existing at all, and belongs in
> `src/assets/cookers/` rather than in the renderer.
>
> And the premise underneath all of it: routing all 26 interface methods through a
> virtual is **~18 ns per tick, 0.00005% of a 30 Hz server tick**. None of this
> is a performance decision ([`headless.md`](headless.md) §1). Not started.

**G7 and G8 are optional tiers, not baseline.** Neither reaches the stated minimum
spec — see [`open-decisions.md`](open-decisions.md) decision 3.

> **G2's backend list was corrected on 2026-09-04** from "D3D12 + Vulkan" to
> "Metal 4 + Vulkan", to match [`design-axioms.md`](design-axioms.md) axiom 6.
> The original line predated the axiom's second rewrite and had been left behind
> — exactly the kind of internal disagreement this split is meant to make visible.

## 9. Effort, honestly

Estimates, explicitly labelled as such, for one engineer working with AI at the
pace this repo has actually moved:

| Phases | Estimate |
|---|---|
| 0–1 (rig + de-contamination) | **3–5 weeks**, and worth doing regardless |
| 2–4 (parity, no per-draw uniforms) | **3–4 months** |
| 5–6 (graph + GPU-driven — the payoff) | **2–4 months** |
| 7–8 (RT, mesh shaders) | **3–6 months**, and open-ended |

So roughly **a year to the full vision, ~6 months to the point where it is
demonstrably better than what we have.** The dominant risk is not API design — it
is the 15 years of driver quirks bgfx absorbs for us, which we will rediscover one
vendor at a time and cannot schedule.
