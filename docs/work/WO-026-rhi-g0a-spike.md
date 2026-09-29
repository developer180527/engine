---
status: plan
id: WO-026
title: RHI G0a spike — answer the specific open questions, then throw it away
program: renderer
priority: P3
size: M
state: parked
parked-until: WO-020 is done (so the spike asks the retained scene's questions), or a free weekend
depends: [WO-020]
touches:
  - docs/rhi/README.md
source: docs/rhi G0a; conversation 2026-09-22
---
## Why
Spikes suit intermittent work: bounded, and thrown away, so there's nothing half-built to come back to.

## Done when
- [ ] each question the spike answers is written down *before* it starts
- [ ] the answers land in `docs/rhi/studies/` as `[measured]`, and the spike code is deleted
