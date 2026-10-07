---
status: plan
id: WO-065
title: The job pool costs nothing when there is no work, measured, and starts as small as the work
program: providers
priority: P2
size: S
state: todo
depends: []
touches:
  - src/core/jobs/jobs_enkits.cpp
  - src/core/jobs/jobs.h
  - src/runtime/services/asset_service.cpp
  - src/assets/cookers/cook_service.cpp
source: review 2026-10-07, item 4; follow-up check of enkiTS's idle behaviour the same day
---
## Why
Every game starts one worker per core (20 here) at boot, whether it has work or not.

The review assumed sleeping workers cost no CPU. That is not established: enkiTS workers spin for a while before they block, so if an empty game hands the pool even one small job a frame, every worker may wake and spin every frame. That would be CPU spent on nothing, hidden as "threads".

Separately, two threads live outside the pool and its priority and count rules: `AssetService`'s loader and `CookService`'s worker.

## Done when
- [ ] measured on an empty project: jobs dispatched per frame, worker wake-ups per second, and CPU time spent spinning
- [ ] if that cost is not negligible (threshold written down from the measurement), the pool starts with as few workers as the work needs and grows on demand (DR-0011), or spinning is bounded, whichever the measurement points at, and the 50k scene is unchanged
- [ ] a frame with no jobs wakes no worker, checked by a test that counts wake-ups
- [ ] the threads outside the pool are listed with why each exists, and each blocks while it has no work (no timed polling), checked

## Contract
Nothing: `jobs::` callers see no change; a pool with fewer workers runs the same jobs.
