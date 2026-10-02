---
status: plan
id: WO-053
title: Skeletons up to 1 024 bones — a bone map per mesh section, splitting what touches more than 128
program: assets
priority: P1
size: L
state: todo
depends: [WO-051]
contracts: [cooker, kit-abi]
touches:
  - src/assets/cookers/mesh/mesh_backend.cpp
  - modules/assetlib/include/assetlib/mesh_asset.h
  - src/animation/skin_palette.h
  - src/core/bone_limit.h
  - src/render/pipeline/opaque_pass.cpp
  - shaders/vs_skinned.sc
  - tests/cooked_format
source: review 2026-10-03 of WO-040 (784efd4)
---
## Why
WO-040 made 128 bones per skinned mesh a hard limit, and production rigs (facial, full body) exceed it, so those characters now fail to cook.

128 is fixed in three places at once: the vertex's 8-bit index into the WHOLE skeleton, the per-entity `mat4[128]` palette slot, and the shaders' `u_boneMatrices[512]`. The skeleton and the draw do not need the same limit. ozz allows 1 024 joints.

## Done when
- [ ] the back end finds, per submesh, the skeleton bones its vertices use, writes a bone map of at most 128 entries, and rewrites vertex indices into it (the vertex format stays 8-bit)
- [ ] a submesh that touches more than 128 bones is split: triangles grouped greedily so each group needs at most 128, vertices on a group boundary duplicated
- [ ] cooked format v7: each submesh records its bone map (offset, count) into a table of 16-bit skeleton indices. The loader treats it as untrusted (every entry < bone count, every map <= 128); the Rust conformance crate and `fuzz_mesh_loader` learn the fields
- [ ] palettes cover the whole skeleton, sized by joint count from size classes (32/64/128/256/512/1024), not a fixed 128
- [ ] each draw gathers its section's bones from the palette into the uniform, main and shadow passes alike; a small mesh uploads only what it uses
- [ ] the cook refuses above 1 024 (ozz's ceiling) by name, not at 128; `MeshCooker::kVersion` bumped; `SkinnedMesh::kMaxBones` changes for kits under WO-051's revision
- [ ] tests: a generated 300-bone rig skinned at rest matches the glTF spec through the contract suite; splitting changes no triangle's skinning; a section touching 129 bones splits into exactly two; the bone map is fuzzed; the palette gather is pinned. Each red with its fix removed
- [ ] measured: frame time and bytes uploaded per frame for 100 animated characters, before and after

## Contract
Changes `cooker`'s output (MeshAsset v7) and `kit-abi` (`SkinnedMesh`'s bone constants).

Nothing: a mesh cooked before v7 is re-cooked (the version bump), as every format change already is.

## Not in scope
- One GPU bone buffer (WO-054).
- Keeping 8 bone influences per vertex instead of 4. Today the extra ones are dropped and reported; add it as an option here only if a real asset needs it.
