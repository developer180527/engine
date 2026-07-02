# Animation

## Purpose
Skeletal animation: skeleton extraction from FBX (via Assimp), clip sampling,
pose blending, and bone-palette computation for GPU linear-blend skinning.

## Architecture
Bones are **not** ECS entities — they live in flat, topologically sorted
arrays (parent index always < child index) for cache-friendly evaluation.

- **`Skeleton`/`Bone`** (`skeleton.h`) — bind pose as SQT *and* as the raw
  `localBindMatrix[16]`, plus `inverseBindMatrix[16]` per bone. `kMaxBones=128`.
- **`AnimClip`/`AnimChannel`** (`animation_clip.h`) — per-bone keyframe
  tracks (translation/rotation/scale), timestamps in seconds.
- **`Pose`** (`pose.h`) — per-bone local SQT plus an `animated` bitmask
  (1=pos, 2=rot, 4=scl) recording which properties a clip actually wrote.
- **`assimp_skeleton_loader.h`** — walks the aiNode tree, collapses
  `$AssimpFbx$` helper chains into single bones, extracts clips and per-vertex
  weights (top-4 influences, normalized).
- **Registries** (`skeleton_registry.h`, `clip_registry.h`) — `Handle<Tag>`
  dense-vector storage, slot 0 reserved as null.
- **`ClipLibrary`** (`clip_library.h`) — clips as STANDALONE assets (the Mixamo
  layout: character FBX + separate clip FBXs). Loads a clip file and BINDS it
  to a target skeleton by bone name (baking `boneIndex` into channels via the
  same `extractAnimClip` path; sampling stays index-only). Cache key is
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
FBX → Assimp (PRESERVE_PIVOTS=false) → extractSkeleton/extractAllClips
  → AnimatorSystem.tick: sampleClip → Pose
  → computeWorldMatrices (parent-chain multiply)
  → computeSkinMatrices: skin[i] = IBM[i] * world[i]
  → SkinnedMesh::skinMatrices → vec4 uniform array → vs_skinned.sc
```

## The Quaternion Convention (critical — root cause of exploded meshes)
Assimp quaternions MUST be **conjugated** (negate xyz) at the import boundary
(`decomposeAiMatrix` for bind, `extractAnimClip` for rotation keys — the two
must stay consistent). Why: `bx::mtxFromQuaternion` emits column-vector-
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
`decomposeAiMatrix → SQT → toMatrix()` remains slightly lossy for matrices
with shear (baked pivot chains). The historical "catastrophic drift" (a jaw
bone at 299 cm) was actually the quaternion-convention bug above; the raw-
matrix paths are kept as the precision-clean baseline:
- Bones with **no** animation channels use `localBindMatrix` raw
  (`pose.isAnimated(i)` check in `computeWorldMatrices`).
- The pure bind pose uses `computeBindPoseWorldMatrices` (raw matrices only),
  guaranteeing `IBM * world_bind ≈ identity`.
Never "simplify" this back to SQT-everywhere.

## Invariants
- Max 128 bones (512 vec4 uniforms), 4 influences/vertex, weights sum to 1.
- Bone indices stored as normalized uint8 in the vertex; decoded in the
  shader with `ivec4(a_indices * 255.0 + 0.5)`.
- Matrices are row-major, bx/bgfx row-vector convention: `child * parent`.
- Animation ticks even when gameplay is paused (editor scrubbing).

## Future Work
- Phase 4: cross-fade transitions, blend trees, layers (the `blendPoses` /
  `additiveBlend` primitives already exist).
- Cook skinned meshes (currently they always take the Assimp fallback path).
