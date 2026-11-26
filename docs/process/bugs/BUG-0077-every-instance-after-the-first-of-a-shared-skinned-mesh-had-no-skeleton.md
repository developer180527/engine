## BUG-0077 — Every instance after the first of a shared skinned mesh had no skeleton
- found:     2026-11-26
- status:    fixed
- class:     logic
- where:     src/runtime/services/asset_service.cpp
- symptom:   in the shipped player, and on the editor's cooked path, only the first entity using a skinned mesh animated; every other entity using the same mesh was drawn in bind pose. Found while writing WO-091's loader, by a test that loads one skinned cooked mesh twice with the skeleton and clip registries wired: the second load came back with no skeleton and no clips.
- cause:     the mesh cache dedups by path, but its entry stored no skin. A cache hit that asked for the skin, with the registries wired, returned the cached handle and left the skin empty; the caller had no skeleton to give SkinnedMesh. Without the registries the same hit instead fell through to a full reload, which also re-read every static mesh's file once per entity whenever a scene load asked for skins (50 000 entities over one mesh: 50 000 reads).
- pinned-by: tests/mesh_ownership_test.cpp
- lane:      unit
- proof:     in AssetService::loadMesh. mesh_ownership_test section 8 failed before the fix ("the second load has them too (skeleton 0, 0 clip(s))") and passes after. The entry now keeps the skeleton and clips its load registered (registered skeletons and clips are never released by unloadMesh, so sharing them changes no lifetime) and records a static mesh as such; a skin-asking hit on either returns the cached handle with its skin. 50 000 entities over one static mesh load in 131 ms instead of 438 ms (tests/scene_load_bench.cpp).
