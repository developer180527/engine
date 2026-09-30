---
status: plan
id: WO-040
title: A rig over the bone limit is refused, not silently truncated
program: assets
priority: P2
size: S
state: todo
contracts: [import-frontend]
touches:
  - src/animation/assimp_skeleton_loader.h
  - src/assets/import/frontend_assimp.cpp
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
- [ ] the Assimp front end reads the whole skeleton; a rig over the limit is dropped as `Skin`/`Wrong` naming its bone count and the limit, so the cook is refused with a reason
- [ ] one limit, stated once and equal to what the runtime palette holds: today the loader and palette say 128 and the back end says 256
- [ ] find out what a cooked 129 to 256-bone rig does at run time today, and pin it with a test
- [ ] a test: a COLLADA rig over the limit is refused with the count in the message, and one at the limit cooks

## Contract
Nothing: a rig the engine cannot skin is refused with a reason, never cooked
with bones missing.
