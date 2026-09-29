---
status: as-built
contract: transform-authority
kind: data
state: provisional
owner: src/runtime
header: src/runtime/transform_authority.h
implementations:
  - real: src/runtime/transform_authority.h
  - null: none
tests:
  - tests/transform_authority_test.cpp
  - tests/physics_authority_test.cpp
covers:
  - src/runtime/transform_authority.h
  - src/components/rigid_body.h
  - src/components/character_controller.h
verified: 2026-09-27
---

# transform-authority — who may write each Transform field

The declared, enforced ownership contract for `Transform` — per FIELD, not per
entity — and a watcher that turns a write by the wrong owner into a named
report. The ECS component contract the other components do not yet have
(`docs/plans/subsystem-contracts.md` §3.1).

## Nothing
`null: none` because the null is a compile level: `ENGINE_TRANSFORM_AUTHORITY=0`
compiles every call out, `1` counts only, `2` records per entity, field and
phase (the debug default). With nothing to watch (no physics components) there
is nothing to report.

## Ownership
Per field:

| entity kind | position | rotation | scale |
|---|---|---|---|
| plain (no physics) | gameplay | gameplay | gameplay |
| RigidBody Static | physics | physics | gameplay |
| RigidBody Dynamic | physics | physics | gameplay |
| RigidBody Kinematic | gameplay | gameplay | gameplay |
| CharacterController | physics | gameplay | gameplay |

Reading is not owning (physics reads a character's rotation and never writes
it). Scale is never backend-owned. Gameplay's legal way to move a physics-owned
pose is `IPhysicsService::teleport`, which moves body, Transform and
interpolation history together and clears velocity.

## Threading
Not yet written.

## Timing
Checked per sim phase; it detects FINAL-STATE ownership violations. It cannot
see a write undone within one phase — the determinism gate catches observable
divergence, and neither catches a transient read-back.

## Errors
A violation is a named report (entity, field, phase, writer), not a crash;
counts at level 1, records at level 2.
