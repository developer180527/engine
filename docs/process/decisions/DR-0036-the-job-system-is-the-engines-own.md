---
status: decided
id: DR-0036
title: The job system is the engine's own work-stealing scheduler, on threads, in standard C++
date: 2026-11-14
source:
  - src/core/jobs/jobs_native.cpp
  - src/core/jobs/info.md
  - tests/jobs_bench.cpp
---

## Decided
`engine::jobs` runs on a scheduler written for this engine
(`jobs_native.cpp`): one lock-free deque per pool thread and priority, work
stealing, three priorities, an I/O lane for blocking work, and per-thread
scratch memory. It uses threads, not fibers, and only standard C++ (no OS
call), so the platform layer can later supply threads and parking without
changing it. enkiTS stays as a second backend (`ENGINE_JOBS_BACKEND=enkits`) for
comparison until the native one has shipped a release.

## Rejected
- **Staying on enkiTS and adding features in the wrapper** (the first
  recommendation, 2026-11-11). Measured, enkiTS wakes every sleeping worker for
  every submission and that cannot be configured: a frame of 6 `parallelFor`s
  and 200 jobs, paced as a game paces it, took 2.03 ms against 0.54 ms, at twice
  the CPU. It also calls OS semaphores directly.
- **Fibers (FiberTaskingLib).** WebAssembly cannot switch stacks; the engine's
  thread-local state (memory tags, allocator stripes, profiler scopes) would
  break when a job resumes on another thread; ThreadSanitizer and crash
  reporting need special handling. Nothing measured asks for them.

## Why
The owner (2026-11-14): the scheduler is critical to frame rate, must not depend
on OS-specific calls, and must be proven by measurement and fuzzing. The numbers
are in `src/core/jobs/info.md`; the proof is `jobs_fuzz_test` on both backends
and under ThreadSanitizer.

## What would reverse it
- **Back to enkiTS:** a correctness defect in the native backend that cannot be
  fixed quickly. That is why the switch is kept.
- **Fibers:** a profile showing workers idle inside waits (deep job graphs, most
  likely from the librhi renderer), on platforms that can switch stacks.
  `jobs.h`'s rules already hold for a fiber backend.
