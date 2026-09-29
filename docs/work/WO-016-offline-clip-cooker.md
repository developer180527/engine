---
status: plan
id: WO-016
title: Offline clip cooker — no clip is cooked at runtime
program: assets
priority: P2
size: M
state: todo
depends: [WO-015]
contracts: [cooker]
touches:
  - src/animation/clip_library.h
source: review 2026-09-29 C4, R2
---
## Why
A standalone clip, such as a Mixamo FBX, is cooked only the first time the editor binds it.

A shipped build that uses a clip nobody played in the editor fails with "clip not cooked". Whether it works depends on what someone clicked.

## Done when
- [ ] a clip cooker in the normal cook pipeline, fingerprinted like the others
- [ ] the cook-on-first-bind path in `clip_library.h` is removed
- [ ] the "animation-only, skipping cook" branch in the mesh cooker routes to the clip cooker instead
- [ ] a test: a project whose clip was never bound in the editor still plays it in a cooked-only runtime

## Contract
Nothing: in a runtime, a missing cooked clip is a WO-018 pending job, not an inline cook.
