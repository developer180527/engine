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
  - src/core/jobs/jobs_shared.cpp
  - src/runtime/runtime_frame.cpp
  - src/runtime/services/asset_service.cpp
  - src/assets/cookers/cook_service.cpp
source: WO-078's findings; review of 2026-11-11; review of 2026-11-14
---
## Why
WO-078 built and proved the scheduler on one machine. What it found and left is listed here so it is not lost: threads that still live outside the job system, a known cost at the start of a frame, numbers that exist for macOS only, and a second backend that should not be carried forever.

## Done when
- [ ] `jobs_bench` and `jobs_fuzz_test` run on the Linux and Windows CI legs, and the numbers are recorded beside the macOS ones (`src/core/jobs/info.md`); `jobs_fuzz_test` runs in CI's ThreadSanitizer job
- [ ] `AssetService`'s worker and `CookService`'s cook thread run on the I/O lane or as `Background` jobs, or the order records why each keeps its own thread
- [x] a wake at frame start (`jobs::frameBegin()` or the same from `engStep`), measured on the paced benchmark: the paced median (0.54 ms) moves toward the back-to-back one (0.15 to 0.28 ms), or the idea is dropped with the number
- [x] `run` takes any callable without `std::function` (small ones stored in the job's block), and the allocation count per job with discarded handles is measured
- [ ] a rule or a guard for waits nested inside jobs (stack depth), decided from a measurement of how deep the engine's real job graphs go
- [ ] `Background` work cannot starve forever: an age limit or a reserved share, with a test
- [ ] the profiler shows the scheduler's stats (`jobs::stats()`: steals, parks, wakes, jobs run by their submitter)
- [ ] enkiTS removed (the backend file, the submodule, the option) once the native backend has been the default through one release with no scheduler defect

## Contract
Nothing: every item is behind `jobs.h` as it stands, except `frameBegin`, which is new and optional.

## Not in scope
Fibers (DR-0036 says what would reopen that).

## Log
- 2026-11-14: a review of WO-078 (spinning, CCX topology, the shared pending counter, starting from idle, allocation per job, range size) and two CI failures, fixed in this order. Numbers are in `src/core/jobs/info.md`.
  - **CI.** Windows: a compound literal in `jobs_fuzz_test.cpp` (C4576). Linux arm64: enkiTS lost a wake-up, a relaxed load in a store-then-load handshake; fixed by `third_party/patches/enkiTS__seq-cst-fences-in-wake-handshake.patch`. It never reproduced on Apple silicon, so the arm64 leg is the confirmation.
  - **Spinning: 20 to 23% of a core to 7 to 8%** on paced frames (new stat, `Stats::spinNanos`). A plain cap of two searchers cut small `parallelFor` from 4.5x to 2.8x; two phases (everyone 10 µs, then two up to 100 µs) kept it.
  - **Waiters have no timer.** They slept with a timeout as a safety net; they now use the same fenced handshake as the workers. A fuzz watchdog (20 s with no progress fails, naming the phase) and a waiter-liveness phase were added first: a planted lost notify hung the fuzz run before them.
  - **`run` allocates nothing in steady state** (`allocs.steady` 0.000; 1.0 on enkiTS). `JobHandle` is an intrusive reference on the job block. Move-only captures now work. `runIo` keeps `std::function`: its jobs take milliseconds.
  - **`frameBegin`: the frame's first job reaches a worker in 1.5 to 1.9 µs instead of 20 µs.** The paced median did NOT measurably move: it varies ±15% between runs here, more than a 20 µs wake-up is worth on a 0.8 ms frame. The item is ticked on the direct measure (`frame.prewake.*.first_worker`); a machine with slower wake-ups (Linux, Windows) should show it in the median too. Costs about 0.1 ms a frame of spinning; a frame that ran no jobs wakes nobody (unit-tested).
  - **The shared pending counter is gone.** Sleepers check the queues instead. `run` 433 to 377 ns/job from one thread, 316 to 350 to 200 to 237 from four. Fuzzed 2 000 seeds; `jobs_test`, 60 fuzz seeds and `stress_jobs` under TSan with no report; a planted sleep-without-looking was caught by the liveness check. A new bench figure, frames in which no worker took part (`.alone`), read 0 of 1 800.
  - **Not done, on purpose:** topology-aware stealing (threads are not pinned; the cost is Zen-specific and needs OS calls, DR-0036), and a different `parallelFor` range size (ranges are claimed dynamically, about eight per thread, which is what the reviewer's fixed-size concern is about).

