---
status: plan
id: WO-043
title: The fixed step reads no wall clock (audit DET-01)
program: providers
priority: P2
size: S
state: todo
touches:
  - src/runtime/runtime_sim.cpp
source: engine audit 2026-09-30; audit DET-01's one accepted finding; src/runtime/docs/info.md's gate-limits table
---
## Why
`tickSimulation` calls `m_input.beginTick(hid::nowNs())`: the moment a tick folds staged input events into its snapshot is the WALL CLOCK. The fixed-tick input snapshot is the multiplayer and replay contract, and anything the tick reads that is not in the recorded input makes a replay, a rollback or a lockstep peer disagree.

It is inert today only because nothing in the determinism gate's tiers reads input (`src/runtime/docs/info.md`). The replay work (WO-038's `sim_replay_test`, kits driving sessions through `engineIntentGet`) is moving towards exactly that.

## Done when
- [ ] the tick boundary for input comes from the simulation's own clock (or the recorded input's timestamps), not `hid::nowNs()`; the live-input path decides which events belong to which tick outside the fixed step
- [ ] audit DET-01 holds with no accepted debt (removed from `scripts/audit_baseline.json`)
- [ ] a determinism-gate tier that reads input, recorded and replayed, agrees tick for tick

## Contract
Tightens the simulation's input contract: a tick is a function of the recorded input and the previous state only.

Nothing: live play is unchanged; a replay of a recorded session reproduces the input the ticks saw.
