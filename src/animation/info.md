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

## Data Flow
```
FBX → Assimp (PRESERVE_PIVOTS=false) → extractSkeleton/extractAllClips
  → AnimatorSystem.tick: sampleClip → Pose
  → computeWorldMatrices (parent-chain multiply)
  → computeSkinMatrices: skin[i] = IBM[i] * world[i]
  → SkinnedMesh::skinMatrices → vec4 uniform array → vs_skinned.sc
```

## The Precision Invariant (important)
`decomposeAiMatrix → SQT → toMatrix()` is **lossy** when FBX pre/post-rotation
is baked in. Error accumulates catastrophically down long bone chains (a jaw
bone once drifted 299 cm). Therefore:
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
