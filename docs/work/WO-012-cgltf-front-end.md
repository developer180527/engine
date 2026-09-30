---
status: plan
id: WO-012
title: cgltf front end (static), retiring cookGltf
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: frontend_cgltf_test (contract suite: 6 pass, 2 skinned skipped for WO-014); old-vs-new comparison on 6 real glTFs (imported-scene.md §7.2); 6 mutations red
depends: [WO-011]
contracts: [import-frontend]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
The first real front end. After this, glTF has no cook path of its own.

## Done when
- [x] a cgltf front end passes the WO-010 contract suite
- [x] `cookGltf` is deleted; `.gltf`/`.glb` go through the front end and the WO-011 back end
- [x] cooked output matches the old `cookGltf` for every glTF test asset **except** the differences expected from `imported-scene.md` §7.1: generated tangents where the file has none, and mirrored instances flipped. Each remaining difference is explained, or it's a bug.
- [x] the WO-002 refusal moves into the front end's dropped list: skins and animations are reported, not silently skipped

## Contract
Nothing: a skinned glTF is still refused (dropped: skins), exactly as in WO-002, until WO-014.

## Log
- 2026-09-30: `imp::CgltfFrontend` (`src/assets/import/frontend_cgltf.*`).
  `MeshCooker::cook` routes `.gltf`/`.glb` through it and the back end, and
  `cookGltf` and its helpers are deleted: `mesh_cooker.cpp` went from 1,084
  lines to 654.
- **Compared before deleting, on real files**, not assumed. A temporary program
  cooked every glTF in the tree through both paths. Geometry, indices,
  submeshes, bounds, materials, LODs and every texture byte were identical.
  Tangents differ everywhere, as intended (no file has `TANGENT`; checked).
  The one surprise was `Television_01_4k`, whose textures were missing on
  **both** sides. Its glTF names a `textures/` folder that the download
  flattened; the old path hid that, and the new one reports it. Full result in
  `imported-scene.md` §7.2. The temporary program and its `WO012_LEGACY_GLTF`
  switch were removed in the same change.
- Contract suite with real files: each case is WRITTEN as a `.gltf` from its
  expected scene. 6 pass; `SkinnedColumn` and `AuthoredCentimetreZUp` are
  skipped and reported, pending WO-014.
- WO-002's refusal lives on as the dropped list: Skin is `Wrong`, and an
  animation-only file is `Empty`. `cooker_test` §2c is unchanged and green.
- Mutations, each red on its own check:
  - node transforms ignored
  - morph targets unreported
  - non-indexed primitives skipped
  - no material → material 0
  - skin only `Less`
  - unresolvable textures passed through (my first attempt at this one did
    not compile; redone)
