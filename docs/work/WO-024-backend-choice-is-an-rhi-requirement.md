---
status: plan
id: WO-024
title: Runtime GPU-backend choice written down as an RHI requirement (not built on bgfx)
program: renderer
priority: P2
size: S
state: done
done: 2026-10-05
evidence: docs/rhi/design-api.md §4.7 (runtime choice from config plus capability, shader blobs per backend, named fallback order, no silent downgrade); cited by phases.md G2; device.cpp comment at the #if
touches:
  - docs/rhi/design-api.md
  - docs/rhi/phases.md
  - src/render/renderer/device.cpp
source: review 2026-09-29 P1
---
## Why
The backend and shader set are picked with compile-time OS checks, so anything that isn't Apple or Windows gets Vulkan.

This is real, but building runtime choice on bgfx means building it twice. So record it as a requirement the RHI must meet, and leave `device.cpp` alone.

## Done when
- [x] `docs/rhi/` gains the requirement: backend chosen at runtime from config plus device capability, with shader blobs looked up per backend and a named fallback order
- [x] the requirement is cited by whichever G-phase implements device creation
- [x] `device.cpp`'s `#if` block has a one-line comment pointing at it

## Not in scope
Changing `device.cpp`.

## Log
- 2026-10-05: done inside WO-056. The requirement is `design-api.md` §4.7,
  written against the tier-L floors study 007 settled (Metal 4 on M1/A14;
  Vulkan 1.3 with descriptor indexing) and tier H as the next fallback if
  built. The location moved from `docs/rhi/README.md` (this order's first
  guess) to `design-api.md`, which is where "the shape of a call" lands
  (workflow §3). G2 cites it. `device.cpp` gets the one-line pointer and is
  otherwise untouched.

