---
status: as-built
tier: working
verified: 2026-08-01
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
| R3 | Shaders are compile-time blobs | **half** — cooking + material assets done; nothing loads them at runtime |
| R4 | Bone palette uploaded per submesh | **open** — still inside `bindMaterial` (`forward_pipeline.h:272`) |
| R5 | No instancing | **open** — `batchRunLength()` exists and is tested; nothing consumes it |
| R6 | Render targets ~5× the naive figure (71 MB) | **mostly explained** — shadow map was the bulk; now a project setting, rt down to 23 MB |
| R7 | Redundant material binds | **open** — no state comparison anywhere in the pipeline |
| R8 | Textures outside the residency system | **done** — texture cache is budgeted and refcounted |
| R9 | No LOD, cascades, occlusion, streaming | **open, deliberately deferred** |

Measured along the way: cook 8 min → 1.8 s; dist 85.7 → 53.6 MB; shipped GPU
peak 71.1 → 63.7 MB with textures actually loading.

---

## 3. The gate: engine-owned default assets

**This is the single decision blocking Phase 5, and it is a design question, not
an implementation detail.**

Three layers are now built and *none of them runs in the engine*:
`.cshader` cooking, `.material` cooking, and `ShaderLibrary`. That is three
consecutive unverified layers, which is exactly the situation to stop and close
before adding a fourth.

To close it, `ForwardPipeline` must load `standard.cshader` instead of
`#include`ing byte arrays. But the engine's shaders live in `/shaders` and are
compiled into the binary at build time, while cooked assets live in a
**project's** `.cache`. So:

> Where do the engine's own cooked default assets live, and how does
> `engine_build` package them into a dist?

Options, with the trade:

1. **Engine default-asset directory** — cook `/shaders` into
   `<build>/engine_assets/` at build time; `engine_build` copies it into the
   dist; `ShaderLibrary` resolves engine shaders there and project shaders in the
   project cache. Clean separation, one extra packaging step, and it gives
   projects a way to *override* an engine default by shipping their own.
2. **Copy engine shaders into every project's assets on scaffold** — simplest
   packaging (no new concept), but every project carries a copy that silently
   goes stale when the engine's shaders change.
3. **Keep compiled-in shaders as the fallback, cooked as the override** — safest
   migration, no packaging work needed to start, but the engine binary keeps
   carrying shaders it may never use.

**Recommendation: 1, with 3 as the transitional state.** Ship the fallback so
the runtime always boots, add the engine-asset directory, then delete the
compiled-in blobs once `fps_shooter` is confirmed rendering from cooked shaders.

---

## 4. What has to be made

### Next — finish Phase 5 (R3)
1. Resolve the packaging decision above.
2. `ForwardPipeline` consumes `ShaderLibrary` for the standard forward program.
3. `AssetService` loads `.cmat`; `Material` gains the data-driven fields;
   the pipeline uploads prebuilt blocks instead of reading `Material::roughness`.
4. Delete the hardcoded fields from `material.h` and the `#include`d shader
   headers. **Not done until this deletion happens** — until then both paths
   exist and the old one is what actually runs.
5. Live-verify `fps_shooter` renders identically.

*Unlocks:* the stated goal — a game defines its look without rebuilding the
engine. `src/render/shader` → `working`.

### Then — Phase 4, submission efficiency
Cheap, because the hooks exist: instancing off `batchRunLength()` (R5), state
dedup against the previous draw (R7), bone palette hoisted out of `bindMaterial`
(R4). All three are contained changes with measurable results.

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
