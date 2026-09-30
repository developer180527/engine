---
status: plan
id: WO-042
title: Re-verify the stale docs, the asset cook architecture first
program: process
priority: P1
size: S
state: done
done: 2026-09-30
evidence: engine_doctor check 0 warnings (was 13); ENGINE_STATUS.md 0 stale docs
touches:
  - docs/architecture/asset-cook-architecture.md
  - src/tools/packaging/info.md
  - src/editor/info.md
source: engine audit 2026-09-30 (engine_doctor: 13 stale docs)
---
## Why
`docs/architecture/asset-cook-architecture.md` is the design's source of truth for cooking, and it was last verified on 2026-08-03. It does not mention `ImportedScene` at all. WO-010 to WO-016 replaced the whole import side with it: the front ends, the one back end, the clip cooker. A reader of the design doc learns an architecture that no longer exists.

`engine_doctor` lists 12 more stale docs. The ones whose code moved most are `src/tools/packaging/info.md` (cooked clips now ship there, WO-016), `src/editor/info.md` (spawn goes through the cooked path, WO-036) and `src/systems/info.md` (the animator's over-limit warning, WO-040).

## Done when
- [x] `asset-cook-architecture.md` describes the import format, its front ends, the one back end and the clip cooker, and points at `docs/plans/imported-scene.md` for the detail
- [x] every doc `engine_doctor check` reports stale is re-read against its code and either corrected or re-verified; `ENGINE_STATUS.md` shows no stale docs

## Log
- 2026-09-30: All 13 docs re-read against the commits that landed under
  them. The ones that were WRONG, not merely unverified:
  - **`asset-cook-architecture.md`**
    - no import side at all: new §2.1 covers `ImportedScene`, the front ends,
      the one back end, the clip cooker, losses, the contract suite and fuzzing
    - §2's layer table used pre-split paths (`src/cook/…`, `src/ddc.cpp`)
    - the overview's key formula omitted declared inputs and dependency hashes
    - cooker versions 12/3 (now mesh 21, texture 4, material 2, shader 1)
    - §3.2's "skipped" example was skinned meshes, which cook now
    - §6.3's "skinned meshes are parsed twice" is done, differently: one parse
      into `ImportedScene`, which is also the natural Phase 1 parse artifact
    - `.cmat` called "inert"; it has been live since 2026-08-01
  - **`src/assets/cookers/material/info.md`:** "mesh-embedded materials
    still use the fixed struct" (Phase 5 step 4 did that), and nothing on how
    a dist resolves material textures (`84aa16e`)
  - **`src/tools/packaging/info.md`:** `resolveMaterialTextures` and
    `packagedClips` missing from its table
  - **`docs/architecture/renderer-vs-production.md`:** "no exposure,
    tonemap" (colour stages B and C), and "none of the rendering is visually
    verified" (skinned animation was checked by eye today)
  - **`docs/plans/renderer-audit-and-plan.md`:** shader and material assets
    listed as future ("impossible today"); both are done
  - **`docs/process/architecture-audit.md`:** §4's state from 09-16 (ABI-03
    fixed, IMP-01 down to 2, DET-01 owned by WO-043), and the IMP-01 row
  - **`src/editor/info.md`:** the cooked spawn path, reveal-per-OS and the
    terminal's quoting were missing
  - **`src/plugins/info.md`:** Jolt's 16-byte realloc (BUG-0068)
  - **`src/systems/info.md`:** the over-limit warning (WO-040)
- **Re-verified with no change needed:**
  - `docs/contracts/platform.md` (a comment-only change under it)
  - `src/project/info.md` (Windows macro guards)
  - `docs/rhi/evidence-bgfx.md` and `evidence-coupling.md`: the renderer
    commits since change nothing they claim
