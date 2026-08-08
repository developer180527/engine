---
status: as-built
tier: working
verified: 2026-08-08
covers:
  - docs/
---
# Roadmap — what exists, what doesn't, and what's next

`ENGINE_STATUS.md` is generated and answers **what is true right now**, per
subsystem, with tiers derived from each `info.md`. This document answers the
question generation cannot: **what is missing, and in what order should it be
built.**

Maturity uses the ladder in `docs/process/engineering-standards.md`:
`prototype → working → hardened → production`. Nothing is `production` yet, and
nothing should be until a shipped game has run on it.

---

## 1. Where the engine stands

**20 tracked subsystems: 4 hardened, 12 working, 4 prototype, 0 production.**

Test lanes: **29 unit, 5 stress, 2 fuzz-regress, 2 docs**; doc contract 56 docs,
0 errors, 0 warnings. Zero `TODO` comments remain in `src/` — the stale ones
were deleted as their work landed rather than accumulating.

### Hardened (has a fuzz/soak/stress lane and a fresh doc)
| Subsystem | What it is |
|---|---|
| `src/core` | Handles, math, logging, profiler channels |
| `src/core/memory` | TLSF tagged heaps, 2 MB blocks, routed global new/delete |
| `src/assets` | Cookers, importers, residency |
| `modules/assetlib` | Registry (SQLite), cook pipeline, DDC, task graph |

The asset/cook stack is the most mature part of the engine by a wide margin —
content-addressed DDC, thermal governance, memory-budgeted admission,
out-of-process crash isolation, GC. It is genuinely studio-shaped.

### Working (real coverage, no adversarial lane yet)
`src/runtime`, `src/runtime/jobs`, `src/scene`, `src/systems`, `src/animation`,
`src/components`, `src/project`, `src/plugins`, `src/render/world`,
`src/render/diag`, `src/assets/cookers/shader`, `src/assets/cookers/material`

### Prototype (works, but thin or unexercised)
| Subsystem | Why it's still prototype |
|---|---|
| `src/render` | The pipeline itself has no test that can fail; a GPU is required to instantiate it |
| `src/render/shader` | Decision logic is tested, but **no running code path executes it yet** |
| `src/editor` | No tests at all |
| `modules/net` | The Rust FFI seam exists; there is no networking |

---

## 2. The renderer — findings from `docs/plans/renderer-audit-and-plan.md`

| # | Finding | State |
|---|---|---|
| R1 | GPU resources had no identity/refcount/dedup | **done** — `GpuResourceCache`, wired for textures through `AssetService` |
| R2 | Extraction/submission split never happened | **done** — `src/render/world/` |
| R3 | Shaders are compile-time blobs | **done for authored content** — a shipped dist renders its standard program from a cooked `.cshader`, and `.cmat` materials load by name. Mesh-EMBEDDED materials still use the fixed struct (Phase 5 step 4) |
| R4 | Bone palette uploaded per submesh | **done** — hoisted to once per item (`pipeline/opaque_pass.cpp`) |
| R5 | No instancing | **done** — runs collapse into instanced submits; 299 covering 41 571 items at 50 k props |
| R6 | Render targets ~5× the naive figure (71 MB) | **mostly explained** — shadow map was the bulk; now a project setting, rt down to 23 MB |
| R7 | Redundant material binds | **done, and measured at zero** (R17) — the dedup is real, the saving was not, because binds were already 1:1 with draws. Kept: it is what makes submesh expansion safe |
| R8 | Textures outside the residency system | **done** — texture cache is budgeted and refcounted |
| R9 | No LOD, cascades, occlusion, streaming | **LOD built (R20), bought nothing measurable** — no decimation in `MeshCooker`, so no level is cheaper than the one above. Cascades / occlusion / streaming still open |

Measured along the way: cook 8 min → 1.8 s; dist 85.7 → 53.6 MB; shipped GPU
peak 71.1 → 63.7 MB with textures actually loading.

---

## 3. The gate that was blocking Phase 5 — resolved

The question was where the engine's own cooked default assets live, since
`/shaders` is engine-owned but `.cache` is project-owned. **Answer: the engine's
shader directory is a second asset root.** `CookService` scans it alongside the
project's assets, so `standard.shader` cooks into each project's `.cache` like
anything else.

- No new packaging concept — `engine_build` already ships `.cache`.
- The duplication is free: every project cooks byte-identical sources to the same
  DDC key, so it is cooked once per **machine**, not once per project.
- A project overrides an engine default by shipping its own.

Two things the implementation had to get right, both found by running it rather
than by reasoning:

**Resolution cannot go through the registry.** `engine_player` sets
`openAssetDatabase = false` — a shipped dist has no `registry.db` at all.
`ShaderLibrary` indexes `<cache>/shaders/*.cooked` by the name each `.cshader`
carries inside itself.

**Ordering.** `openProject()` runs after `Renderer::init()`, so the pipeline had
already attached against an empty cache. `setShaderCacheRoot()` re-attaches, or
the whole path silently no-ops.

Verified in a shipped `fps_shooter` dist: `standard program: cooked .cshader`,
5 textures resident, gameplay intact.

## 4. What has to be made

### Next — finish Phase 5 (R3)
1. ~~Resolve the packaging decision.~~ **done** — §3
2. ~~`ForwardPipeline` consumes `ShaderLibrary`.~~ **done** — verified in a dist
3. ~~`AssetService` loads `.cmat`; the pipeline uploads prebuilt blocks.~~
   **done** — materials resolve by authored name, verified in a dist.
4. **The remaining step.** Mesh-EMBEDDED materials (`CookedMaterial` inside
   cooked geometry) still fill the fixed struct, and that is what every surface
   in `fps_shooter` currently renders through — so this is a MIGRATION, not a
   deletion: `AssetService` must synthesize a data-driven Material against the
   standard shader at mesh load, and only then do the fixed fields have no
   writer and delete cleanly. Same for the compiled-in program fallback.
   **Phase 5 is not finished until both are gone** — until then two paths exist
   and either could be the one that runs.
5. Live-verify `fps_shooter` renders identically.

*Unlocks:* the stated goal — a game defines its look without rebuilding the
engine. `src/render/shader` → `working`.

### ~~Then — Phase 4, submission efficiency~~ — **done** (R17–R20)
All three landed: instancing off `batchRunLength()` (R5), material-bind dedup
(R7), bone palette hoisted to once per item (R4). Submesh-granular visible sets
(R18) were the prerequisite that made the first two work on real content —
50 000 props went from *3 067 draws + 534 refused, frame incomplete* to **299
draws covering 41 571 items**.

Two results were negative and are recorded as such: R7's dedup saved nothing
(binds were already 1:1 with draws) and R20's LOD bought nothing measurable (no
decimation, so no level is cheaper). Both were kept — the dedup is what makes
submesh expansion safe, and LOD is correct and waits on a decimator.

### Then — render graph (tier-2 customization)
Deliberately after Phase 5. The engine has **two passes**; a render graph for two
passes is machinery in search of a problem. What creates the need is
post-processing, which is tier-2 customization and only worth building once
tier-1 (material + shader) is proven.

### Standing gaps, not yet scheduled
| Gap | Cost of leaving it |
|---|---|
| **Windows build** | The stated target (i3-10105 / UHD 630 / 4 GB) cannot run anything yet. Also: shader cooking has no `CreateProcess` path, and D3D bytecode needs a Windows runner. |
| **Scene-deserializer fuzzer** | Blocks `src/runtime` and `src/scene` from `hardened`; both parse untrusted input |
| **`src/editor` has no tests** | Largest untested surface in the tree |
| **Networking** | `modules/net` is an FFI seam with nothing behind it. Transport scope undecided. |
| **Texture UUIDs in `.cmat`** | Materials declare no dependency edge on their textures |
| **Shader hot-reload** | Cook pipeline already watches files; the runtime ignores the result |

### Explicitly not being built
- A material node graph. Features are a closed list the shader author declares —
  this is the rule that keeps the permutation matrix cookable in full, with no
  stripping infrastructure. See `src/assets/cookers/shader/info.md`.
- PSO baking. bgfx exposes no PSO API and every backend already caches
  internally (`renderer_vk.cpp:2165`, `renderer_d3d12.cpp:3341`,
  `renderer_mtl.cpp:2563`).
- A fiber job system. Dropped; enkiTS is sufficient at this scale.

---

## 5. The honest summary

The **offline half** of this engine is strong: the cook pipeline is the most
mature subsystem and would hold up in a studio. The **online half** is where the
risk sits — `src/render` is the least-verified subsystem in the tree and the one
carrying the hardest requirement (60 FPS on a 128 MB-VRAM iGPU).

The immediate priority is not more architecture. It is **closing the loop on the
three built-but-unused layers**, so that what has been designed is also known to
work.
