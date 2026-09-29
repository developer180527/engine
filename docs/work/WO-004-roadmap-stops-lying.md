---
status: plan
id: WO-004
title: The roadmap stops lying about where we are
program: process
priority: P1
size: S
state: todo
touches:
  - docs/process/roadmap.md
  - docs/README.md
source: found 2026-09-22 — roadmap says 20 subsystems/6 hardened, ENGINE_STATUS says 22/9
---
## Why
`docs/README.md` tells you to read the roadmap second, and its numbers are a month old.

Nothing caught it: its `covers: docs/` points at a directory rather than the code its claims are about, so the staleness check has nothing to compare against.

## Done when
- [ ] the roadmap's live numbers are removed and replaced with a pointer to `ENGINE_STATUS.md`. A copied number is a number that will go stale.
- [ ] its "what next" ordering is replaced with a pointer to `docs/work/BOARD.md`. The roadmap keeps strategy: what is missing, and why that order.
- [ ] `docs/README.md` "start here" reads: `work_orders.py brief`, then `ENGINE_STATUS.md`, then the board, then the roadmap
- [ ] the roadmap's `covers:` names something that actually changes when it should be re-read, or it becomes `status: decided`, so staleness is honest either way
