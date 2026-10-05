---
status: plan
id: WO-026
title: RHI G0a baseline — what the bgfx path costs at 50 000 objects, so the RHI has a number to beat
program: renderer
priority: P2
size: S
state: todo
depends: [WO-020]
touches:
  - docs/rhi/README.md
  - docs/rhi/phases.md
source: docs/rhi G0a; conversation 2026-09-22; re-scoped 2026-10-05 by DR-0012
---
## Why
G0a was a go/no-go spike for the whole RHI. DR-0012 took that job away: bgfx
has no bindless, and the RHI is a library for more than this engine. What is
still missing is the number the new backend must beat, measured the same way
it will be measured later.

## Done when
- [ ] the 50 000-object fuzz scene measured on bgfx's current path: CPU frame, extraction, submit, GPU time, and memory mapped by the renderer, recorded as `[measured]` in `docs/rhi/studies/`
- [ ] the same scene and counters are a repeatable bench (not a one-off), so G4 reruns it against the RHI
- [ ] no compute or indirect spike code is written: that question is closed by DR-0012
