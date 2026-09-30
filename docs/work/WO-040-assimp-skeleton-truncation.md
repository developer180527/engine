---
status: plan
id: WO-040
title: A rig over the bone limit is refused, not silently truncated
program: assets
priority: P2
size: S
state: done
done: 2026-09-30
evidence: mesh_backend_test (128 cooks, 129 refused by name); frontend_assimp_test §5 and frontend_cgltf_test §5 (300-bone rig read whole; 65,546-joint skin Skin/Wrong); render_pipeline_test (shader palette literal = kMaxBones*4); four mutations red
contracts: [import-frontend]
touches:
  - src/core/bone_limit.h
  - src/animation/assimp_skeleton_loader.h
  - src/assets/import/frontend_assimp.cpp
  - src/assets/import/frontend_cgltf.cpp
  - src/assets/cookers/mesh/mesh_backend.cpp
  - src/runtime/services/async_loader/parse.cpp
  - src/systems/animator_system.h
source: review 2026-09-30 (found while making extractSkeleton iterative, WO-039)
---
## Why
`anim::extractSkeleton` keeps the first `kMaxBones` (128) skeleton nodes and
drops the rest, and the only sign is a line on stderr. The Assimp front end
then builds its skeleton from what is left. Weights that named a dropped bone
lose it, so a large rig (a MetaHuman has 800+ bones) cooks broken, and nothing
in the cook log says so.

The cook back end refuses more than 256 bones with a named error, but the
truncation happens before that check can see the real count. And 256 is not
the runtime's limit: the bone palette holds 128 (`animation/skeleton.h`,
`skin_palette.h`, `components/skinned_mesh.h`). So a glTF rig of 129 to 256
bones, which the cgltf front end reads whole, cooks. What the runtime then does
with it has not been checked.

## Done when
- [x] the Assimp front end reads the whole skeleton, every influence included; a rig over the limit is refused by the cook back end naming its bone count and the limit. (Refused in the back end, not dropped by the front end: the limit is the cook target's, and the front end's job is to carry the file. The one limit a front end applies is the format's own, 65,536 bones for `uint16` joints, as `Skin`/`Wrong`.)
- [x] one limit, stated once and equal to what the runtime palette holds: `core/bone_limit.h`, used by the skeleton, the palette pool, `SkinnedMesh`, the animator, the bone uniform and the back end; the shaders' literal is checked against it
- [x] find out what a cooked 129 to 256-bone rig does at run time today, and pin it with a test: the animator refused it (`hasSkinMatrices = false`), so it drew in its raw bind pose, never animated, with no message. The cook now refuses it (pinned), and cooker v20 makes rigs cooked under the old limit recook and be refused.
- [x] a test: a rig over the limit is refused with the count in the message, and one at the limit cooks (`mesh_backend_test`); a 300-bone COLLADA and glTF rig are read whole

## Contract
Nothing: a rig the engine cannot skin is refused with a reason, never cooked
with bones missing.

## Log
- 2026-09-30: The limit was stated six times: `skeleton.h`, `skin_palette.h`,
  `SkinnedMesh`, the animator, the bone uniform (a literal 512) and the back end
  (256). The back end's disagreed. They now all come from
  `core/bone_limit.h`. The shaders' `u_boneMatrices[512]` is a literal no
  C++ constant reaches, so `render_pipeline_test` reads both skinning shaders
  and checks it.
- There was a second silent loss under the first: `extractBoneWeights` stored
  joint indices as `uint8` and skipped every influence on a bone past the
  256th. It stores `uint16` now, as `ImportedScene` does. The uncooked preview
  narrows to the GPU's `uint8` only after checking the skeleton is within
  `kMaxBones`, and loads a larger rig as static geometry, with a warning.
- The animator, which still refuses an over-limit skeleton (a kit or the SDK
  could hand it one), now says so once per skeleton.
- Mutations, each red, with the sources restored:
  - truncation to 128 put back: 128 bones read, the top vertices on the wrong bone
  - the >255 influence skip put back: the top vertices lose 'Bone299'
  - the back end's limit set back to 256: 129 bones cook
  - no `uint16` check in cgltf: the 65,546-joint skin is not refused
- Not pinned by a dedicated test: the uncooked preview's static fallback and
  the animator's warning. Both are a few lines on paths WO-018 removes or
  rarely reaches; a test would need a COLLADA writer outside
  `frontend_assimp_test` for the first, and a log capture for the second.
