---
status: plan
id: WO-055
title: Fuzz the cooked clip format a shipped game reads
program: assets
priority: P2
size: S
state: todo
contracts: [cooker]
touches:
  - src/animation/cooked_clip.h
  - tests/fuzz_cooked_skin_test.cpp
source: review 2026-10-03 of WO-016 (3a3b180)
---
## Why
Since WO-016, clips are standalone cooked files a shipped game reads from disk, and nothing fuzzes them.

`decodeCookedClip` checks bounds on every read and runs ozz's `Validate`. But its digest is stored in the file, so it stops corruption, not a crafted file, and ozz's deserializer allocates from counts it reads. The ozz archives inside cooked meshes got `fuzz_cooked_skin` for exactly this reason.

## Done when
- [ ] `fuzz_cooked_skin_test` (or a sibling target) mutates cooked clips, digest recomputed after each mutation so the archive itself is reached
- [ ] no crash, no unbounded allocation, and every accepted clip binds to a skeleton and samples finite poses
- [ ] run under ASan and UBSan, as the sanitizer job now runs fuzz-regress; any finding fixed with a seed in the corpus

## Contract
Nothing: a refused clip is "clip not cooked", as today.
