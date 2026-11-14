---
status: plan
id: WO-084
title: Days-long soak, fuzzing at scale and the determinism lane on the farm
program: process
priority: P2
size: L
state: todo
depends: [WO-082]
touches:
  - tests/jobs_fuzz_test.cpp
source: docs/plans/farm-and-perf-ci.md §5.3-5.5; automated-testing-soak-fuzz-plan.md Parts 2 and 6.3
---
## Why
Leaks, slow growth and rare races show up over days, not in a test lane's minutes. And lockstep multiplayer needs the same simulation on x86-64 and arm64.

## Done when
- [ ] a soak program on the C API: a churning scene (spawn and destroy, scene load and unload, physics, animation, heavy jobs), recording memory per tag, mapped bytes, frame percentiles, threads, file descriptors and job stats every minute
- [ ] it stops and keeps evidence on a crash (core dump), a sanitizer report, a hang past a watchdog limit, or a steady slope in memory or descriptors
- [ ] `jobs_fuzz_test` and the `fuzz-explore` targets with fresh seeds through the night, alternating ASan and TSan builds; failures deduplicated, minimised and written as corpus candidates
- [ ] the determinism lane: per-tick world hashes of a fixed scene from the server, kept so the Mac can compare its own

## Contract
Nothing.
