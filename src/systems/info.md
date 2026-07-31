---
status: as-built
tier: working
verified: 2026-07-31
covers:
  - src/systems/
tests:
  - tests/animator_system_test.cpp
# Raised prototype -> working 2026-07-31: animator_system_test now asserts
# AnimatorSystem's OWN behavior (time advance + speed, looping wrap incl. the
# negative case, clamp-and-auto-stop both directions, bind-pose fallback,
# cleared flag on a missing skeleton) and — the reason it was written — the
# >kMaxBones guard. Deleting that guard does not merely fail an assertion:
# the 129-bone bind-pose path overruns `float worldMatrices[128*16]` and the
# stack protector aborts the process (SIGABRT, exit 134). Verified by
# deliberate removal.
---
# Systems

## Purpose
Per-frame ECS systems that are part of the engine core (not plugins).

## Contents
- **`AnimatorSystem`** (`animator_system.h`) — queries `Animator +
  SkinnedMesh`, advances clip time (looping/clamping, speed scale), samples
  the clip into a `Pose`, computes world + skin matrices into
  `SkinnedMesh::skinMatrices` for the renderer to upload. Falls back to the
  lossless bind-pose path when no clip is assigned (see
  `src/animation/info.md` for why that path avoids SQT).

## Rules
- Systems run in `EngineRuntime::tickSystems`, before rendering.
- Animation ticks even while gameplay is paused (editor preview/scrub).
- Guard against skeletons exceeding `kMaxBones` (128) — set
  `hasSkinMatrices=false` rather than overflowing the palette.

## Future Work
- More engine systems land here as they're promoted from editor/plugin code
  (e.g. transform hierarchy propagation, LOD selection).
