---
status: decided
id: DR-0001
title: Kinematic bodies are gameplay-owned in both directions
date: 2026-09-09
source:
  - src/plugins/info.md
  - docs/process/bugs/BUG-0058-a-kinematic-body-could-not-be-moved.md
---

## Decided
A `Kinematic` body's pose belongs to gameplay, reading and writing. Physics
moves it toward the pose gameplay sets, and `JoltPlugin::writeBackTransforms`
never writes a kinematic body's pose back into its `Transform`. Only `Dynamic`
bodies are written back.

## Rejected
The plan's original authority table, which made Kinematic physics-owned like
Dynamic: physics would write its integrated pose back every step.

## Why
Writing back round-trips gameplay's own value through a velocity integration
that does not land exactly on its target. Measured: 5.0 comes back as
4.99999952. So a gameplay script that set a position read back a different
one, and a body set to a pose drifted from it (BUG-0058). Implementing the
original table is what showed it was wrong.

## What would reverse it
A kinematic body type whose pose physics legitimately owns (a physics-driven
platform with no gameplay target), or a write-back that lands bit-exactly on
the target. Either would be a new body type, not a change to this one.
