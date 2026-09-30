---
status: plan
id: WO-043
title: The fixed step reads no wall clock (audit DET-01)
program: providers
priority: P2
size: S
state: done
done: 2026-09-30
evidence: sim_clock_test (the boundary function; a catch-up frame's press/release/press lands one per tick, and with the clock read put back it is HPR H-- H--); audit DET-01 ok with no baseline entry; determinism_gate_test 0 divergences
touches:
  - src/runtime/runtime_sim.cpp
  - src/runtime/sim_clock.h
  - src/runtime/input/input_manager.cpp
source: engine audit 2026-09-30; audit DET-01's one accepted finding; src/runtime/docs/info.md's gate-limits table
---
## Why
`tickSimulation` calls `m_input.beginTick(hid::nowNs())`: the moment a tick folds staged input events into its snapshot is the WALL CLOCK. The fixed-tick input snapshot is the multiplayer and replay contract, and anything the tick reads that is not in the recorded input makes a replay, a rollback or a lockstep peer disagree.

It is inert today only because nothing in the determinism gate's tiers reads input (`src/runtime/docs/info.md`). The replay work (WO-038's `sim_replay_test`, kits driving sessions through `engineIntentGet`) is moving towards exactly that.

## Done when
- [x] the tick boundary for input comes from the simulation's own clock (or the recorded input's timestamps), not `hid::nowNs()`; the live-input path decides which events belong to which tick outside the fixed step: the frame's pump time (`InputManager::lastPumpNs`, read once per frame, before draining) and the accumulator (`simclock::inputTickEndNs`)
- [x] audit DET-01 holds with no accepted debt (removed from `scripts/audit_baseline.json`)
- [x] a determinism-gate tier that reads input, recorded and replayed, agrees tick for tick: the gate's `input` tier (same motion at two frame rates, identical world hashes over 240 ticks) and `sim_replay_test` (a child replays a take tick for tick) both pass; see the log for what they do and do not exercise

## Contract
Tightens the simulation's input contract: a tick is a function of the recorded input and the previous state only.

Nothing: live play is unchanged; a replay of a recorded session reproduces the input the ticks saw.

## Log
- 2026-09-30:
  - **Where the clock went.** The frame reads it once: `InputManager::pump()`
    stamps `lastPumpNs` before draining, and no staged event is newer. After
    each step's decrement, the accumulator holds exactly the real time not
    yet simulated, so the step's window ends that much before the pump
    (`simclock::inputTickEndNs`). Windows end `kSimDt` apart and never run
    backwards. `runtime_sim.cpp` contains no `nowNs()` at all: the audit rule
    works per file, and the editor-preview path uses the pump time too.
  - **It was a real bug, not only a finding.** In a catch-up frame the first
    step's "now" folded every staged event, so a click whose press and
    release fell in one long frame lost its release: `sim_clock_test` reads
    HPR H-- H-- with the old line put back. Now it reads HP- --R HP-.
  - **What the replay claim rests on.** Boundaries are derived from REAL
    pump times, which is right for live play. A replay is tick-exact because
    takes record the per-tick intents, after the boundaries have done their
    work; `sim_replay_test` checks that. The gate's `input` tier stamps
    events small enough that they always fold on arrival, so it does not
    exercise boundaries; `sim_clock_test` does. The gate's comment and its
    reported finding now say so.
  - **Not fixed, and filed as WO-045.** The intent's look delta diffs a
    total that grows at pump time, so a catch-up frame's mouse motion still
    goes to its first step. That is a fidelity issue, not a determinism one.
