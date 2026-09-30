---
status: plan
id: WO-045
title: A catch-up frame's look motion is spread over its steps, not given to the first
program: providers
priority: P3
size: S
state: todo
touches:
  - src/runtime/runtime_sim.cpp
  - src/runtime/input/input_manager.cpp
source: found while doing WO-043, 2026-09-30
---
## Why
Since WO-043 each fixed step's input window has its own boundary, so in a catch-up frame (several steps back to back) keys and buttons reach the step whose slice of time they happened in.

The intent's LOOK delta does not follow. `sampleLocalIntent` diffs `InputManager::lookTotal`, a cumulative total that grows at PUMP time, by design: it is what makes the look frame-rate independent across ordinary frames. But in a catch-up frame the whole frame's motion lands in the first step and the rest see none. A heading that feeds movement (the FPS kit's shape, and the determinism gate's `input` tier) turns in one step instead of three.

It is not a determinism problem: a take records the per-tick intents, so a replay reproduces whatever the live session did. It is a feel and fidelity one, and only on frames long enough to need catching up.

## Done when
- [ ] the look delta per tick is the motion whose timestamps fall in that tick's window (the snapshot's `mouseDx/Dy`, or the total diffed at the window's boundary), so a catch-up frame spreads it
- [ ] the same motion at 1 and 2 frames per tick still yields the same intents (`sim_intent_test` §4), and `sim_clock_test` gains a look case like its button case

## Contract
Nothing: an ordinary frame (one step) delivers the same look delta as today.
