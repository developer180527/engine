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
  - scripts/farm/
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

## Log
- 2026-11-14: **a soak runner for the existing `soak_engine`**, ahead of the C-API soak program this order asks for (WO-081's scripts build it). `scripts/farm/farm-soak.sh` runs it for `FARM_SOAK_MINUTES` (2 880) from `bin/release/current`, samples resident memory, peak, threads and descriptors each minute (`metrics.csv`), and stops with the evidence on a `FAIL` line, a crash or sanitizer report, no output for 10 minutes (SIGABRT, so the core has every stack), resident growth over 8 MB/h after the first hour, or descriptor growth over 32. A finished run or a newer build starts the next; a finding leaves the unit stopped until someone looks (`farm-soak-status.sh`). Not ticked: it is `soak_engine`, not the C-API program, and it has not run on the box.
