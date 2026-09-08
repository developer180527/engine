---
status: as-built
tier: working
verified: 2026-09-08
covers:
  - src/systems/
tests:
  - tests/animator_system_test.cpp
  - tests/determinism_gate_test.cpp
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
  the entity's slot in `anim::skinPalettes()` for the renderer to upload — the
  palette left the component (it was 8 200 bytes of stride the extraction query
  paid for and never read), so the component keeps a `paletteSlot` and the
  animator takes one lazily on first write. The slot comes back through a
  `SkinnedMesh` remove hook that `init()` and `tick(world, dt)` install on EVERY
  world the animator touches, because hooks are world state and the play snapshot
  world is where entities actually die (`src/runtime/docs/issues.md`). Falls back to the
  lossless bind-pose path when no clip is assigned (see
  `src/animation/info.md` for why that path avoids SQT).

## Advance and sample are separate phases

`AnimatorSystem` has two halves, and which half a piece of work belongs to is
decided by one question: **does the simulation own it?**

| | mutates | runs |
|---|---|---|
| `advance(dt)` | `Animator::time`, `Animator::playing`, `AnimContext` crossfade clocks | in the FIXED step, at `kSimDt` |
| `sample()` | ozz sampling contexts, the bone palette, `SkinnedMesh::hasSkinMatrices` | once per FRAME |
| `tick(dt)` | both | editor preview only — no session, so no fixed step to hang a clock on |

`Animator::time` is a hashed component (`components/sim_state.h`), so it is
simulation state. The bone palette is not — `SkinnedMesh` is `SimExempt`
("paletteSlot is a render-pool allocation").

Before the split both ran in one `tick()` called at frame rate, so
`Animator::time` advanced at render rate: the same content simulated at 1
frame/tick and 2 frames/tick diverged on the first tick.

**How that is known**, because the first version of this paragraph overstated
it: `tests/determinism_gate_test.cpp` has an `animator` tier that builds skinned
entities through ozz's offline builders, and reverting the split makes that
tier's A/B comparison diverge at tick 0. It is in the gating lane, so the split
cannot regress. The gate did *not* originally catch this — it found `Spinner`,
the same defect two lines away in `tickSystems`, and the animator was inferred
from proximity until the tier was built.

Two things the split is careful about, both of which a naive version gets wrong:

* **The fade clocks moved with the rest.** `ctx.fadeElapsed` and `ctx.prevTime`
  used to be advanced *between* the two `SamplingJob`s. They are clocks, so they
  advance in `Phase::Advance`; the sampler recomputes `alpha` from them and
  advances nothing. Leaving them where they were would double-step a crossfade
  whenever the frame rate and the tick rate differ.
* **The advance phase does not touch presentation.** An entity with a missing or
  oversized skeleton keeps exactly its old behaviour, including not advancing
  its clock — `hasSkinMatrices` is only written in `Phase::Sample`.

The split also stops a hitch multiplying the expensive half: the accumulator can
run four fixed steps in one frame, and the old shape sampled four poses to show
one.

## Rules
- Systems run in `EngineRuntime::tickSystems`, before rendering.
- Animation ticks even while gameplay is paused (editor preview/scrub).
- Guard against skeletons exceeding `kMaxBones` (128) — set
  `hasSkinMatrices=false` rather than overflowing the palette.

## Future Work
- More engine systems land here as they're promoted from editor/plugin code
  (e.g. transform hierarchy propagation, LOD selection).
