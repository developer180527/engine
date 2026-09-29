---
status: plan
id: WO-022
title: Decision records — the "why" survives the person
program: process
priority: P2
size: M
state: todo
touches:
  - docs/process
source: conversation 2026-09-22 — how big teams keep context
---
## Why
Important decisions are buried in the middle of plan paragraphs, and those are the ones that get "fixed" back into bugs.

Example: Kinematic bodies are gameplay-owned in *both* directions, because physics write-back returns 5.0 as 4.99999952. In three months that code will look wrong, and the reason will be somewhere in stage 3a of a plan file.

## Done when
- [ ] `docs/process/decisions/` with one file per decision, each four headings long: decided / rejected / why / what would reverse it (`status: decided`)
- [ ] an index, generated like the board
- [ ] backfilled with at most the 10 decisions most likely to be undone by accident (kinematic ownership, LUT-after-encode, extended linear sRGB not P3, no AgX, bx in the kit ABI, no renderer ABI freeze, …)
- [ ] a plan that says "we decided" links a decision id. This is a check in `work_orders.py` or the doctor.

## Not in scope
Backfilling everything. New decisions get records as they're made; old ones only when someone trips on them.
