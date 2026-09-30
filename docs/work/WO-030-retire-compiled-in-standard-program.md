---
status: plan
id: WO-030
title: Retire the compiled-in standard program
program: renderer
priority: P3
size: M
state: parked
parked-until: we decide that a cooked shader cache is a hard prerequisite of running at all (a deployment decision)
touches:
  - src/render/pipeline/programs.cpp
source: docs/process/roadmap.md, old §4 "Next: finish A5", item 4 remainder and item 5
---
## Why
Every mesh-embedded material still draws with the compiled-in standard program. It started as a fallback and is now the main path.

Retiring it needs two things. The cooked standard shader must gain an instanced variant, or every mesh-embedded material drops out of instanced runs (3 067 draws instead of 299 at 50 k props). And a cooked shader cache must become required to run.

## Done when
- [ ] the cooked `standard.shader` has an instanced variant, and mesh-embedded materials stay in instanced runs (draw count at 50 k props is unchanged)
- [ ] the compiled-in standard blob is deleted from `programs.cpp`, and a missing cooked shader cache is a startup error that names the fix
- [ ] `fps_shooter` renders with no visible change. This was the old roadmap's unchecked item 5, and it can only be checked against a reference captured *before* this change.
