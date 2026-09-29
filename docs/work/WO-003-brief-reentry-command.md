---
status: plan
id: WO-003
title: One command to regain context — `brief`
program: process
priority: P1
size: S
state: todo
depends: [WO-001]
touches:
  - scripts/work_orders.py
source: conversation 2026-09-22 — "I lose context VERY quickly and cannot work daily"
---
## Why
Coming back after a week should take one command, not an afternoon of re-reading.

`work_orders.py next` already answers *what next*. `brief` adds what changed since you last worked, whether the lanes are green, and what is uncommitted. Every line is derived from git or the tree, so none of it can be stale.

## Done when
- [ ] `python3 scripts/work_orders.py brief` prints: last commit and its age; commits since a given ref (default: the last commit by you before today); uncommitted file count grouped by top-level area; the `next` output; the stale docs from `ENGINE_STATUS.md`; open questions whose paths overlap the active order's `touches:`
- [ ] runs in under 2 s with no build and no network
- [ ] `docs/work/README.md` opens with `brief` instead of `next`

## Not in scope
Running the test lanes. `brief` reads results; it never starts builds.
