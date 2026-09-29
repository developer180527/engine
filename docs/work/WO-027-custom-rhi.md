---
status: plan
id: WO-027
title: Custom RHI implementation
program: renderer
priority: P3
size: XL
state: parked
parked-until: WO-019, WO-020 and WO-026 are done, then split into G-phase orders of size L or less
depends: [WO-019, WO-020, WO-026]
contracts: [gpu-seam]
touches:
  - docs/rhi/README.md
source: conversation 2026-09-22 — "should we start the RHI?"
---
## Why
The largest and least reversible piece of work in the engine, with no working middle stage.

Its shape depends on what the retained renderer asks of it, so building it first means designing for a consumer we're about to replace.

## Done when
- [ ] split into G-phase orders, each of size L or less, each ending in something that runs

## Contract
Nothing: the existing `gpu-seam` null backend stays the headless answer throughout.
