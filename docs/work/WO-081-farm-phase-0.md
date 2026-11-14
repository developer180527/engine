---
status: plan
id: WO-081
title: The home server, phase 0 — pulls main, builds four trees, quiet enough to measure, noise floor known
program: process
priority: P2
size: M
state: todo
depends: [WO-080]
touches:
  - tests/corebench/CMakeLists.txt
  - scripts/farm/
source: docs/plans/farm-and-perf-ci.md §3-4; automated-testing-soak-fuzz-plan.md §7
---
## Why
Every number the server will produce is compared against a baseline, and a comparison means nothing until the machine is quiet and its noise is measured. The repository is public, so the server must take work by pulling, never by being handed it.

## Done when
- [ ] `scripts/farm/` holds the pull-and-build script and its systemd unit and timer; it fetches `main` anonymously, builds only a new commit, and keeps the last build of each tree
- [ ] four trees build on the box: Release, Release with profiling, ASan+UBSan, TSan
- [ ] machine tuning applied and written down (governor, turbo, isolated cores, swap, `perf_event_paranoid`, `systemd-coredump`), with a script that prints the current state for each result
- [ ] a lock so a benchmark never runs while a build or fuzz run does
- [ ] the noise floor for `tests/corebench`: the same binary against itself, 7 runs, median and spread per metric, recorded
- [ ] no GitHub credentials on the box, stated in the script's header

## Contract
Nothing: the box only reads the repository.

## Not in scope
The results database and report (WO-082), engine workloads (WO-083), soak and fuzz at scale (WO-084).

## Log
- 2026-11-14: **the scripts exist; nothing is ticked, because none has run on the box yet.** `scripts/farm/`: `farm-setup.sh` (root, once: packages, the `farm` user, `/srv/engine-farm`, an anonymous clone, the units), `farm-update.sh` (flock, fetch `main` over https and refuse any other origin, build `release`/`asan`/`tsan` incrementally, copy the binaries to `bin/<tree>/<sha>/` and move `current` only on success, keep five), `farm-tune.sh` (governor, turbo off, `perf_event_paranoid=1`, systemd-coredump; prints the `isolcpus` GRUB line), `farm-state.sh` (the machine as key=value, saved with every build and run), systemd units with a nightly-plus-hourly timer. Written on the Mac, shellchecked, not run on Linux. Still to do here: run them on the box, the profiling tree, the noise floor.
