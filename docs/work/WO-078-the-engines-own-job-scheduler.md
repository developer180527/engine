---
status: plan
id: WO-078
title: The engine's own job scheduler — work stealing, priorities, an I/O lane, scratch memory; measured and fuzzed
program: process
priority: P1
size: L
state: done
done: 2026-11-14
evidence: tests/jobs_fuzz_test.cpp (60 seeds in the unit lane, 1 500 passed; both backends; ThreadSanitizer clean on 190 seeds; two planted bugs caught); tests/jobs_bench.cpp (a paced frame 0.54 ms against enkiTS's 2.03 ms at half the CPU, numbers in src/core/jobs/info.md); unit, audit and docs lanes on the native backend
depends: []
touches:
  - src/core/jobs/jobs.h
  - src/core/jobs/jobs_native.cpp
  - src/core/jobs/jobs_shared.cpp
  - src/core/jobs/jobs_enkits.cpp
  - src/core/frame_arena.h
  - src/CMakeLists.txt
source: owner 2026-11-14; review of 2026-11-11 (memory and jobs); DR-0036
---
## Why
The job system decides how much of a frame runs in parallel, so its fixed costs are paid many times a frame. Measured, the enkiTS backend woke every sleeping worker on every submission: one `run` cost 2.5 to 10 µs, and a frame of 6 `parallelFor`s and 200 jobs took 2 to 3 ms. It also had no priorities, no place for blocking work, no scratch memory for jobs, a limit of 8 foreign threads, and it called OS semaphores directly, which the engine's rule on OS independence forbids.

## Done when
- [x] a baseline measured before any change (`tests/jobs_bench.cpp`), and the same benchmark run on both backends after
- [x] a native backend behind the unchanged `jobs.h` contract, in standard C++ with no OS call: lock-free deque per thread and priority, work stealing, wake only what is needed, idle workers asleep
- [x] three priorities (`Frame`, `Normal`, `Background`); callers that pass none behave as before
- [x] an I/O lane (`runIo`) for blocking work, and the asset loader and the audio device start moved onto it
- [x] scratch memory per thread (`scratchAlloc`, `ScratchScope`), nesting-safe, from the engine heap
- [x] `run` reaches the allocator rarely in steady state (0.79 allocations per job in the benchmark, where every handle is kept; 1.00 before)
- [x] any number of foreign threads, with no registration (enkiTS: 8 slots)
- [x] the job system compiled into `engine_core`; `FrameArena` backed by the engine heap
- [x] a fuzz test against `jobs.h` only, passing on both backends, clean under ThreadSanitizer, and shown to catch planted bugs (one of them only after a liveness check was added)
- [x] enkiTS kept selectable (`ENGINE_JOBS_BACKEND=enkits`), and the decision recorded (DR-0036)
- [x] an idle pool measured at no CPU (0.02 ms in 500 ms)

## Contract
Nothing: `jobs.h`'s existing calls keep their signatures and meaning. New arguments have defaults.

## Not in scope
See WO-079 for what was found and left: the engine's remaining private threads, a wake-up at frame start, `run` without `std::function`, numbers on other platforms, and retiring enkiTS.

## Log
- 2026-11-14:
  - **The first plan was wrong and the measurement said so.** The recommendation on 2026-11-11 was to keep enkiTS and add features in the wrapper. The baseline showed its wake policy was the cost, and that cannot be configured from outside.
  - **A locked deque was tried first** (a spinlock per queue): 1.2 µs per `run` with one producer and eleven thieves on one lock. The lock-free deque took it to 0.55 µs.
  - **Waking only what is needed** was not enough: one producer of short jobs still paid for a wake-up on each (800 000 in the benchmark). Not waking while a worker is already searching took that to about 25 000.
  - **Waiters parked too early**, costing each `parallelFor` a wake-up at its tail; they now look for work as long as an idle worker does.
  - **The fuzz test missed a planted lost wake-up twice**: a waiting thread helps, which hides it, and with three workers another sleeper takes the wake. The liveness check uses one worker and nobody helps.
