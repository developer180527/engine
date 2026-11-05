---
status: decided
id: DR-0029
title: Core is fixed and private, the ECS is core; providers are swappable behind contracts
date: 2026-10-29
source:
  - ARCHITECTURE.md
  - docs/plans/engine-architecture.md
---

## Decided
Core (memory, jobs, the ECS and world, scenes, assets and loading, the frame
loop, platform and input, logging and profiling) is fixed: no game replaces
it. Providers (rendering, physics, audio, scripting, networking, UI, and new
kinds as named services) are chosen per project, each behind a contract with a
null version and a conformance suite. **The ECS is core and not swappable**:
flecs is used directly inside core, and never appears at the public boundary
(DR-0028).

## Rejected
Wrapping flecs everywhere so the ECS could be swapped: it costs query speed
and ergonomics for a swap nobody wants. Also rejected: everything swappable,
which leaves nothing that makes this a game engine.

## Why
The owner (2026-10-29). The parts every game needs are where the engine's
value is, so they are fixed and optimised; the parts games legitimately choose
differ per game and per platform, so they are swappable. The same boundary
keeps flecs, bgfx and Jolt out of what users compile against.

## What would reverse it
Replacing the ECS becoming a real goal (a measured limit of flecs that no
upgrade fixes).
