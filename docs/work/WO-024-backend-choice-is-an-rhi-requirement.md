---
status: plan
id: WO-024
title: Runtime GPU-backend choice written down as an RHI requirement (not built on bgfx)
program: renderer
priority: P2
size: S
state: todo
touches:
  - docs/rhi/README.md
  - src/render/renderer/device.cpp
source: review 2026-09-29 P1
---
## Why
The backend and shader set are picked with compile-time OS checks, so anything that isn't Apple or Windows gets Vulkan.

This is real, but building runtime choice on bgfx means building it twice. So record it as a requirement the RHI must meet, and leave `device.cpp` alone.

## Done when
- [ ] `docs/rhi/` gains the requirement: backend chosen at runtime from config plus device capability, with shader blobs looked up per backend and a named fallback order
- [ ] the requirement is cited by whichever G-phase implements device creation
- [ ] `device.cpp`'s `#if` block has a one-line comment pointing at it

## Not in scope
Changing `device.cpp`.
