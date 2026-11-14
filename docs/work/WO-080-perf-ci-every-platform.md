---
status: plan
id: WO-080
title: The core measured and fuzzed in CI on six platforms — Windows and Linux on x64 and arm64, macOS arm64, Android
program: process
priority: P1
size: S
state: todo
depends: []
touches:
  - .github/workflows/perf.yml
  - tests/corebench/CMakeLists.txt
  - tests/corebench/summarise.py
  - tests/jobs_bench.cpp
source: owner 2026-11-14; docs/plans/farm-and-perf-ci.md §2
---
## Why
The new job scheduler (WO-078) was measured on one Mac. Its numbers and its fuzz proof have to hold on every platform the engine ships to, and the measurement has to run without anyone remembering to.

## Done when
- [ ] `perf.yml` green on all five desktop legs: both backends fuzzed, `mem_test` passing, five interleaved benchmark runs per backend uploaded
- [ ] the Android leg green (arm64-v8a built, x86_64 run on the emulator), then its `continue-on-error` removed
- [ ] the first run's summary table copied into `src/core/jobs/info.md` beside the macOS numbers, with the core counts of each runner
- [ ] anything the first run finds on a platform fixed, or recorded here with the reason it waits

## Contract
Nothing: a new workflow and a standalone project; the engine build is untouched.

## Not in scope
Engine-level benchmarks and absolute baselines: the home server's job (WO-081 to WO-084), because shared runners are too noisy for them.
