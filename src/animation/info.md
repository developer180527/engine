---
status: as-built
tier: working
verified: 2026-08-26
parses-external-input: true
covers:
  - src/animation/
tests:
  - tests/anim_pose_test.cpp
  - tests/clip_binding_test.cpp
  - tests/import_test.cpp
---
# Animation

## Purpose
Skeletal animation: skeleton extraction from FBX (via Assimp), clip sampling,
pose blending, and bone-palette computation for GPU linear-blend skinning.

`skin_palette.h` owns the palettes themselves — 8 KB each, out of the ECS
component and into a slot pool with chunked, never-reallocated storage, because a
pointer handed to the renderer must stay valid while the GPU upload reads it.
`at()` is deliberately LOCK-FREE (an atomic chunk-pointer table): extraction calls
it once per skinned item from inside `jobs::parallelFor`, and the mutex it used to
take serialized every worker thread on the one path the pool exists to speed up.

## The ozz Backbone
Sampling/blending machinery is **ozz-animation** (third_party/ozz-animation,
same philosophy as Jolt/bgfx/flecs — orchestrate, don't reinvent):
- `ozz_bridge.h` — THE seam: `buildOzzSkeleton` (engine Skeleton -> ozz runtime
  skeleton + ourBone->ozzJoint map), `buildOzzClip` (Assimp curves -> compressed
  ozz Animation, name-bound, rest-pose keys for unanimated joints).
- `AnimClip` wraps `ozz::animation::Animation`; `Skeleton` carries the ozz
  skeleton + joint mapping. The hand-rolled AnimChannel sampler is deleted.
- AnimatorSystem runs SamplingJob -> LocalToModelJob, remaps ozz joints to OUR
  bone order, then IBM * model (pose.h) into the GPU palette. Per-entity ozz
  contexts live in the system (components get snapshot-copied), keyed by
  entity id, dropped with the world cache.
- Conventions: ozz = Assimp (column-vector) — clip quats feed ozz UNCONJUGATED;
  rest poses conjugate our stored bind SQT back; ozz Float4x4 memory is
  byte-identical to bx row-vector layout (no transpose at the seam).
- ozz jobs are scheduler-agnostic: the future job system parallelizes
  animation by handing each entity's jobs to workers.

## Architecture
Bones are **not** ECS entities — they live in flat, topologically sorted
arrays (parent index always < child index) for cache-friendly evaluation.

- **`Skeleton`/`Bone`** (`skeleton.h`) — bind pose as SQT *and* as the raw
  `localBindMatrix[16]`, plus `inverseBindMatrix[16]` per bone. `kMaxBones=128`.
- **`AnimClip`** (`animation_clip.h`) — wraps a compressed
  `ozz::animation::Animation` + name/duration + track-mapping diagnostics.
- **`pose.h`** — the two surviving raw-matrix helpers: bind-pose world
  matrices + IBM multiply (the precision-clean baseline for skinning).
- **`assimp_skeleton_loader.h`** — walks the aiNode tree, collapses
  `$AssimpFbx$` helper chains into single bones, extracts per-vertex weights
  (top-4 influences, normalized). Clip extraction lives in `ozz_bridge.h`.
- **Registries** (`skeleton_registry.h`, `clip_registry.h`) — `Handle<Tag>`
  dense-vector storage, slot 0 reserved as null.
- **`ClipLibrary`** (`clip_library.h`) — clips as STANDALONE assets (the Mixamo
  layout: character FBX + separate clip FBXs). Loads a clip file and BINDS it
  to a target skeleton by bone name (via `anim::buildOzzClip`). Cache key is
  (path | skeleton handle); unmapped tracks warn (first few named); zero
  mapped tracks = wrong rig, refused. MUST import with the same Assimp
  settings as `async_loader.cpp` (`PRESERVE_PIVOTS=false`) or rotation tracks
  land on `$AssimpFbx$` helper names that don't exist on the skeleton.
  `Animator::clipPath` carries the reference (AssetRef uuid+relative on disk);
  the scene-load import callback binds it once the skinned mesh arrives.
  Owned by EngineRuntime (`clipLibrary()`), reachable via RuntimeContext.
  Regression: `clip_binding_test <character.fbx> <clip.fbx>`.

## Data Flow
```
FBX → Assimp (PRESERVE_PIVOTS=false) → extractSkeleton → buildOzzSkeleton
                                     → buildOzzClip (per animation)
  → AnimatorSystem.tick: ozz SamplingJob → ozz LocalToModelJob
  → remap ozz joints → our bones; skin[i] = IBM[i] * model[ozzJointOf[i]]
  → anim::skinPalettes()[slot] → vec4 uniform array → vs_skinned.sc (mul(v,M))
```

## The Quaternion Convention (critical — root cause of exploded meshes)
Assimp quaternions MUST be **conjugated** (negate xyz) at the import boundary
(`decomposeAiMatrix` for bind — ozz-bound clip keys stay UNCONJUGATED, since
ozz shares Assimp's convention; `ozz_bridge.h` owns that seam). Why: `bx::mtxFromQuaternion` emits column-vector-
convention memory into our row-vector pipeline (`aiMat4ToFloat16` transposes;
`mtxMul(world, local, parent)` is v·L·P), so an unconjugated Assimp quaternion
recomposes as the INVERSE rotation. Diagnosed numerically: `toMatrix(bindSQT)`
vs raw `localBindMatrix` had per-element error 1.47 on rotated bones (toes,
thumbs) — invisible at bind pose (raw-matrix fallback masked it), catastrophic
the moment a clip actually played (skin translations of 200–380 cm ≡ the
"exploded zombie"). After the fix the round-trip error is 0.000 and a
near-bind take gives skin ≈ identity. Regression: `anim_pose_test`.

## The Palette Layout Contract (GPU handoff)
Bone palettes upload as a RAW `vec4[512]` uniform array — bgfx does NOT prep
raw arrays the way it preps `u_model`, so they arrive in bx row-major memory
untouched. The skinned shaders therefore use the ROW-VECTOR multiply
(`mul(v, skin)`, see vs_skinned.sc). Uploading with `mul(skin, v)` renders
exploded meshes even with a bit-perfect palette (diagnosed empirically with
identity/transposed-palette runs + screenshots). If the palette pipeline is
ever changed, keep `anim_pose_test`'s CPU-skin check AND an on-screen look —
CPU-correct does not imply GPU-correct here.

## The Precision Invariant (still applies)
SQT decomposition is slightly lossy for matrices with shear (baked pivot
chains), so the no-clip bind pose renders through the raw-matrix path
(`computeBindPoseWorldMatrices`, guaranteeing `IBM * world_bind ≈ identity`)
rather than any SQT round-trip.

## Crossfade (AnimatorSystem)
Changing `Animator.clip` while a clip is already bound auto-starts a crossfade
over `Animator.fade` seconds (0 = hard cut). The outgoing clip keeps advancing
during the fade; both are sampled per frame and mixed with ozz `BlendingJob`
(rest pose as the fallback layer) before the single `LocalToModelJob`. Each
entity holds two `SamplingJob::Context`s behind `unique_ptr` (the context type
is not movable) that swap roles on a clip switch — the incoming clip pays one
cold-cache frame, which is fine. `engineAnimPlay` is the C-API front door:
resolve path → `ClipLibrary` bind (cached) → set `Animator.clip`, and the
switch detection does the rest.

## Invariants
- Max 128 bones (512 vec4 uniforms), 4 influences/vertex, weights sum to 1.
- Bone indices stored as normalized uint8 in the vertex; decoded in the
  shader with `ivec4(a_indices * 255.0 + 0.5)`.
- Matrices are row-major, bx/bgfx row-vector convention: `child * parent`.
- Animation ticks even when gameplay is paused (editor scrubbing).

## The Clip Cooker (cook-on-first-bind)
ClipLibrary persists every successful bind as an ozz Animation archive under
`<project>/.cache/anim/<hash(source|skeletonSig)>.ozzclip` (small invalidation
header: source size+mtime, joint-name signature, format version — bump
kCookVersion on format change). Later binds and later RUNS deserialize in
~0.1ms instead of a ~20-50ms Assimp parse. Names serialize inside the ozz
animation (raw.name set in ozz_bridge). Hosts enable it via setCacheRoot at
project open; bare tools run pure-Assimp.

## Future Work
- Data-driven state machines as a client of the crossfade + engineAnim* API.
- Eager cook mode (engine_cook walks scene clip refs) + async bind path
- Skeleton/skinned-mesh cooking (mesh FBX still parses via Assimp at load): emit ozz archives (skeleton/animation serialization) so
  the runtime loads pre-built data instead of bridging Assimp at import.
- Cook skinned meshes (currently they always take the Assimp fallback path).
- Parallel evaluation via the job system (ozz jobs are scheduler-agnostic).
