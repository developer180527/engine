---
status: plan
id: WO-011
title: One cook back end — ImportedScene to cooked assets
program: assets
priority: P2
size: L
state: done
done: 2026-09-30
evidence: src/assets/cookers/mesh/mesh_backend.cpp; tests/mesh_backend_test.cpp (38 checks, reads cooked bytes back); 8 mutations each red
depends: [WO-010]
contracts: [cooker]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
Vertex baking, normal matrices, tangents, materials and texture resolution get written **once**, against `ImportedScene`, instead of once per parser.

## Done when
- [x] back end turns an `ImportedScene` into `MeshAsset` v2 (static) and **v6** (skinned; this order said v3, and the skinned format has been v6 since it gained blob digests)
- [x] tested through `ImportedScene` values only, with no real file parsed
- [x] refuses a scene whose dropped list contains something the target format needs (WO-009): a `Wrong` loss, an invalid scene, or more than 256 bones
- [x] the bar for WO-012/013 is recorded, and **corrected**: byte-identical *except* the deliberate differences in `imported-scene.md` §7.1. The three old paths contradict each other, so no single rule can reproduce all three.

## Contract
Nothing: while front ends are unported, the old per-parser paths stay live behind the extension dispatch. The back end is additive until WO-012 and WO-013 switch each format over.

## Log
- 2026-09-30: `meshcook::cookImportedScene` (`mesh_backend.cpp`) writes, once,
  everything the three per-parser paths wrote three times. It covers static
  baking, skinned bind space, tangents, materials, sibling textures, the ozz
  skeleton and clips, and LODs. It is additive: `MeshCooker::cook` is unchanged.
- **Shared helpers moved rather than copied**: LODs, sibling-texture writing,
  the normal-matrix guard and ozz draining now live in `cook_common.*`, used
  by the old paths and the back end alike. `cooker_test` is unchanged and
  green across the move. `ozz_bridge.h` gained `finishOzzClip`, the
  format-independent half of clip building, which both paths now call.
  `imp::Material` gained `roughness`/`metallic`, which every old path already
  cooked.
- **Reading the old paths in full found more disagreements than WO-009 had**
  (plan §7.1):
  - static Assimp stores an embedded texture as the basename `"*0"`, which
    resolves to nothing
  - skinned Assimp forces tangent `w` to +1, and silently drops meshes that
    have no weights
  - no path fixed mirrored instances, which render inside-out
  - a bare external-texture basename likely never resolves on the cooked path
    (suspected; WO-013 must prove it)

  The back end chooses one rule each and says why.
- `mesh_backend_test` (38 checks) reads the cooked bytes back. The clip is
  **sampled** to prove its rotation keys were not conjugated, and a rotated
  bind bone is read back to prove it was.
- Mutations, each red on its own check:
  - mirrored winding kept
  - mirror not flipping tangent `w`
  - bind rotation not conjugated
  - clip keys conjugated
  - texture dedup off
  - `Wrong` losses cooked
  - rigid meshes bound to the root
  - no tangent generation
