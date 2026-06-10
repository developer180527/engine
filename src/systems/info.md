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
