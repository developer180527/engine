---
status: plan
id: WO-045
title: A catch-up frame's look motion is spread over its steps, not given to the first
program: providers
priority: P3
size: S
state: done
done: 2026-10-01
evidence: sim_clock_test §3 (look per tick (10,1) (20,2) (34,-1) in a three-step frame; the old per-tick diff, restored as a mutation, gives (64,2) (0,0) (0,0) and fails 3 checks; an ordinary frame still delivers all its motion); sim_intent_test §4 and the determinism gate input tier unchanged; 136 tests, audit and doctor clean
touches:
  - src/runtime/runtime_sim.cpp
  - src/runtime/input/input_manager.cpp
  - src/runtime/input/input_manager.h
  - src/runtime/runtime.h
  - tests/sim_clock_test.cpp
source: found while doing WO-043, 2026-09-30
---
## Why
Since WO-043 each fixed step's input window has its own boundary, so in a catch-up frame (several steps back to back) keys and buttons reach the step whose slice of time they happened in.

The intent's LOOK delta does not follow. `sampleLocalIntent` diffs `InputManager::lookTotal`, a cumulative total that grows at PUMP time, by design: it is what makes the look frame-rate independent across ordinary frames. But in a catch-up frame the whole frame's motion lands in the first step and the rest see none. A heading that feeds movement (the FPS kit's shape, and the determinism gate's `input` tier) turns in one step instead of three.

It is not a determinism problem: a take records the per-tick intents, so a replay reproduces whatever the live session did. It is a feel and fidelity one, and only on frames long enough to need catching up.

## Done when
- [x] the look delta per tick is the motion whose timestamps fall in that tick's window (the snapshot's `mouseDx/Dy`, or the total diffed at the window's boundary), so a catch-up frame spreads it
- [x] the same motion at 1 and 2 frames per tick still yields the same intents (`sim_intent_test` §4), and `sim_clock_test` gains a look case like its button case

## Contract
Nothing: an ordinary frame (one step) delivers the same look delta as today.

## Log
- 2026-10-01:
  - **Not quite the fix this order named.** Taking only the window's motion
    in EVERY step (the snapshot's `mouseDx/Dy`, or the total diffed at the
    boundary) would change ordinary frames: motion stamped between the
    step's boundary and the pump would wait a tick, adding look latency and
    breaking the contract. So a step that is not its frame's last takes its
    window, and the frame's LAST step takes everything pumped. A one-step
    frame is unchanged.
  - **A queue, not a subtraction.** The boundary sum can't be derived from
    `lookTotal`: `pump()` reads the clock before draining, so an event can be
    stamped after the pump time, and several sources interleave out of time
    order. `InputManager` keeps a tick-look queue of (time, dx, dy) in integer
    counts. It is on only while a session runs with a local controller
    (`EngineRuntime::syncTickLook`), so it can't grow unread. It starts empty,
    which does what re-basing the old cursor did at session start. During a
    replay it is emptied unread each step. `consumeLook` and `lookTotal` are
    untouched for their other consumers.

