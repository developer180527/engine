---
status: plan
id: WO-042
title: Re-verify the stale docs, the asset cook architecture first
program: process
priority: P1
size: S
state: todo
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
- [ ] `asset-cook-architecture.md` describes the import format, its front ends, the one back end and the clip cooker, and points at `docs/plans/imported-scene.md` for the detail
- [ ] every doc `engine_doctor check` reports stale is re-read against its code and either corrected or re-verified; `ENGINE_STATUS.md` shows no stale docs
