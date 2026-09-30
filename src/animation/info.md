---
status: as-built
tier: hardened
verified: 2026-09-30
parses-external-input: true
covers:
  - src/animation/
tests:
  - tests/fuzz_cooked_skin_test.cpp
  - tests/anim_pose_test.cpp
  - tests/clip_binding_test.cpp
---
# Animation

## This module knows no source format (WO-015)
Nothing in `src/animation/` includes Assimp or any parser (audit IMP-01, no
debt). Clips and skeletons reach it as engine types:
- `ozz_bridge.h`'s `finishOzzClip(raw, skel, name, mapped, total)` is the one
  place a clip is built: rest-pose keys for joints the clip leaves alone,
  validation, and the ozz build.
- `imp::buildOzzClip(const imp::Clip&, const Skeleton&)` and
  `imp::toAnimSkeleton(const imp::Skeleton&)` (`assets/anim_from_scene.h`)
  convert the import format and call it. The mesh cook back end and the
  standalone-clip reader both use them, so every clip is built identically.
- The Assimp-typed helpers (`extractSkeleton`, `extractBoneWeights`) live in
  the Assimp front end (`assets/import/frontend_assimp_skeleton.h`). WO-018
  deleted the two runtime importers that also used them, and the legacy
  `buildOzzClip(aiAnimation*)` only they called.

Rotation keys are **not** conjugated (ozz uses the source convention), and bind
rotations **are** (`toAnimSkeleton`). `mesh_backend_test` samples a cooked clip
to pin the first, and reads a rotated bone back to pin the second. Moving the
standalone-clip path onto the front end was checked on the 12 Mixamo zombie
clips: every joint's model matrix, sampled 25 times per clip, is bit-identical
to the old direct-Assimp path.

## The untrusted-input boundary, and why this is still `working`

A cooked `.mesh` carries two **opaque third-party archives** — an
`ozz::animation::Skeleton` and one `ozz::animation::Animation` per take — and
`cooked_skin.h` hands those bytes straight to `ozz::io::IArchive` on the
AsyncLoader worker thread. That is this subsystem's `parses-external-input: true`,
and until 2026-08-29 nothing fuzzed it. `fuzz_mesh_loader_test` fuzzes the mesh
*container* and stops at the blob, which is opaque to it and never decoded.

`tests/fuzz_cooked_skin_test.cpp` closes that, and found a real defect on its
**second case** (BUG-0045): ozz's `IArchive` is documented as trusting its input
— *"reading cannot fail"* — and enforces it with debug-only asserts, so a
truncated archive **aborted in debug and was silently accepted in release**, built
out of partially uninitialised memory. Fixed by `anim::GuardedStream` plus a patch
removing ozz's read-side asserts, so both builds now agree and refuse.

The explore lane then found a second, deeper class (BUG-0046) that the stream
guard **structurally cannot catch**: a corrupt COUNT field that ozz reads
successfully — nothing comes up short — and then allocates from, tripping its own
sanity check. That one held the tier at `working` until it was fixed properly.

> **The fix is format integrity, because ozz cannot be made safe from the
> outside.** Mesh format **v6** writes an FNV-1a digest ahead of each opaque
> blob. `loadMesh` fails the whole load on a mismatch; `decodeCookedSkeleton` and
> `decodeCookedClips` re-check it before touching ozz. **A missing digest is
> refused, not trusted** — a pre-v6 skinned mesh must be re-cooked, which
> `MeshCooker::kVersion` 16 → 17 makes automatic through the DDC.
>
> `hardened` now, on that evidence: 20 000 explore cases pass where the pre-fix
> build aborted at case 34, and both repro seeds are in the gating corpus.
>
> What this does NOT defend against, so nobody assumes otherwise: an attacker who
> rewrites a blob can rewrite the digest beside it. FNV-1a detects CORRUPTION —
> a partial write, a bad disk, a truncated copy. The defence against a hostile
> source is the DDC's content hash.

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
  skeleton + ourBone->ozzJoint map), `finishOzzClip` (source keys -> compressed
  ozz Animation, rest-pose keys for unanimated joints). Format-free.
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
  `localBindMatrix[16]`, plus `inverseBindMatrix[16]` per bone. `kMaxBones=128`, defined once in `bone_limit.h` (the GPU palette's size; `render_pipeline_test` checks the shaders' literal against it). A rig over it is refused by the cook, loaded static by the uncooked preview, and not animated, with a warning, by the animator; nothing truncates it (WO-040).
- **`AnimClip`** (`animation_clip.h`) — wraps a compressed
  `ozz::animation::Animation` + name/duration + track-mapping diagnostics.
- **`pose.h`** — the two surviving raw-matrix helpers: bind-pose world
  matrices + IBM multiply (the precision-clean baseline for skinning).
- **Registries** (`skeleton_registry.h`, `clip_registry.h`) — `Handle<Tag>`
  dense-vector storage, slot 0 reserved as null.
- **`ClipLibrary`** (`clip_library.h`) — clips as STANDALONE assets (the Mixamo
  layout: character FBX + separate clip FBXs). Loads a clip and BINDS it to a
  target skeleton by bone name. A cooked clip is read here; an uncooked one
  comes from the host's SOURCE READER (`setSourceReader`): the runtime installs
  `imp::readSourceClip` (`assets/clip_source.h`) when it has source importers,
  which imports the file through its format's front end (so it reads exactly as
  the character's cook does: same front end, same `PRESERVE_PIVOTS=false`) and
  binds its first clip. With no reader (shipping, server) an uncooked clip is
  an error. Cache key is (path | skeleton handle); unmapped tracks warn (first
  few named); zero mapped tracks = wrong rig, refused.
  `Animator::clipPath` carries the reference (AssetRef uuid+relative on disk);
  the scene-load import callback binds it once the skinned mesh arrives.
  Owned by EngineRuntime (`clipLibrary()`), reachable via RuntimeContext.
  Regression: `clip_binding_test <character.fbx> <clip.fbx>`.

## Data Flow
```
source file → import front end → ImportedScene → imp::toAnimSkeleton → buildOzzSkeleton
                                               → imp::buildOzzClip (per clip) → finishOzzClip
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

## The Clip Cooker (WO-016)
Every animation-only source (a Mixamo clip FBX, a glTF of animations) is cooked
by the normal pipeline, whether or not anyone plays it: the mesh cooker routes a
scene with clips and no triangles to `clipcook::cookClip`
(`assets/cookers/clip/`). The output is an `anim::CookedClip`
(`cooked_clip.h`): the clip's keys BY BONE NAME, skeleton-independent, so one
cook serves every character on the rig. Binding (`anim::bindRawClip`, the same
code every clip is built with) happens in `ClipLibrary::load`, in memory.

How the runtime finds it: the editor's `ClipLibrary` asks the asset registry
(a locator the runtime installs); a shipped build has no registry, so
`engine_build` copies every cooked clip to
`.cache/anim/<assetlib::packagedClipFileName(source path)>`, which `ClipLibrary`
finds from the source path a cooked scene stores. `clip_cook_test` runs both
paths over a clip nothing ever bound.

It replaced cook-on-first-bind: `ClipLibrary` wrote an `.ozzclip` per (source,
TARGET skeleton) the first time the editor played a clip, so a shipped build had
exactly the clips someone had played. The format is guarded like the mesh's
ozz blobs: a digest checked before ozz reads a byte, a GuardedStream, and
structural checks after (`fuzz_cooked_clip_test`).

## Future Work
- Data-driven state machines as a client of the crossfade + engineAnim* API.
- Async bind path (WO-018: a missing cooked clip becomes a pending job)
- Skeleton/skinned-mesh cooking (mesh FBX still parses via Assimp at load): emit ozz archives (skeleton/animation serialization) so
  the runtime loads pre-built data instead of bridging Assimp at import.
- Cook skinned meshes (currently they always take the Assimp fallback path).
- Parallel evaluation via the job system (ozz jobs are scheduler-agnostic).
