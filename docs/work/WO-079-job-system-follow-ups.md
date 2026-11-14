---
status: plan
id: WO-079
title: Job system follow-ups — the last private threads, a frame-start wake, other platforms, retiring enkiTS
program: process
priority: P2
size: M
state: todo
depends: []
touches:
  - src/core/jobs/jobs.h
  - src/core/jobs/jobs_native.cpp
  - src/runtime/services/asset_service.cpp
  - src/assets/cookers/cook_service.cpp
source: WO-078's findings; review of 2026-11-11
---
## Why
WO-078 built and proved the scheduler on one machine. What it found and left is listed here so it is not lost: threads that still live outside the job system, a known cost at the start of a frame, numbers that exist for macOS only, and a second backend that should not be carried forever.

## Done when
- [ ] `jobs_bench` and `jobs_fuzz_test` run on the Linux and Windows CI legs, and the numbers are recorded beside the macOS ones (`src/core/jobs/info.md`); `jobs_fuzz_test` runs in CI's ThreadSanitizer job
- [ ] `AssetService`'s worker and `CookService`'s cook thread run on the I/O lane or as `Background` jobs, or the order records why each keeps its own thread
- [ ] a wake at frame start (`jobs::frameBegin()` or the same from `engStep`), measured on the paced benchmark: the paced median (0.54 ms) moves toward the back-to-back one (0.15 to 0.28 ms), or the idea is dropped with the number
- [ ] `run` takes any callable without `std::function` (small ones stored in the job's block), and the allocation count per job with discarded handles is measured
- [ ] a rule or a guard for waits nested inside jobs (stack depth), decided from a measurement of how deep the engine's real job graphs go
- [ ] `Background` work cannot starve forever: an age limit or a reserved share, with a test
- [ ] the profiler shows the scheduler's stats (`jobs::stats()`: steals, parks, wakes, jobs run by their submitter)
- [ ] enkiTS removed (the backend file, the submodule, the option) once the native backend has been the default through one release with no scheduler defect

## Contract
Nothing: every item is behind `jobs.h` as it stands, except `frameBegin`, which is new and optional.

## Not in scope
Fibers (DR-0036 says what would reopen that).
