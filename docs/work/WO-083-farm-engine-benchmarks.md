---
status: plan
id: WO-083
title: Engine benchmarks and profiling on the farm — headless workloads through the C API, instructions retired, flame graphs
program: process
priority: P2
size: L
state: todo
depends: [WO-082]
touches:
  - src/capi/eng_capi.cpp
source: docs/plans/farm-and-perf-ci.md §5.1-5.2
---
## Why
The core benchmarks say how fast the job system is. A game is physics, animation, extraction, loading and cooking, run through the same frame the player runs.

## Done when
- [ ] workloads, each a program on the C API (`engCreateEngine`, `engStepWithDelta`): the 2 000-box physics pile, N animated characters, a 50 000-object scene's extraction and culling, scene load cold and warm, and cook cold, warm and no-op
- [ ] for the 50 000-object scene: what a headless engine skips of extraction (`canPresent()` false) found and written down, so it is not mistaken for measured
- [ ] `perf stat` instructions, cycles, cache and branch misses recorded with every timing
- [ ] `perf record` flame graphs per workload per commit, kept and linked from the report
- [ ] the engine's profiler channels (frame stats, memory per tag, `jobs::stats()`) written to the database

## Contract
Nothing.
