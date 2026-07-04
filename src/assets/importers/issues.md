# Multi-submesh import — RESOLVED (Fri Jul 4 2026)

All three issues below shared one root cause: the IMPORT path never adopted the
multi-submesh representation the cooked/runtime path already used (one `Mesh`
with a shared VB/IB + a `SubmeshRange` per part, each with its own material,
drawn as multi-draw by the forward pipeline). The fix converges the importers
onto that representation and fixes a latent render-time material bug.

## glТF importer — FIXED
`gltf_importer.cpp` now walks **every** mesh and primitive (not just
`meshes[0].primitives[0]`), packing them into one shared VB/IB with a
`SubmeshRange` per primitive; materials are resolved once and cached by pointer.
Proof: `person.glb` now imports 26 submeshes (was 1).

## Assimp importer — FIXED
`assimp_importer.cpp` now MERGES every triangle submesh into one `Mesh`
(base-vertex-offset indices, a `SubmeshRange` + material each) instead of
creating N separate meshes and returning only the first. Skinned models merge
into one skinned buffer; a bone-less submesh binds rigidly to bone 0 so it can't
collapse. Proof: `Double_Dagger_Stab.fbx` imports 11 skinned submeshes with
distinct materials (was 1).

## Asset pipeline — RESOLVED without a signature change
`MeshImportResult` keeps its single `MeshHandle` — the multi-ness lives INSIDE
the `Mesh` (its `submeshes`), so no pipeline/serialization/component-layout
change was needed. A single-submesh model stays on the simple single-draw path.

## Bonus: renderer per-submesh material (was latent)
`forward_pipeline.h` multi-drew submesh RANGES but bound the mesh's one material
for all of them — so even cooked multi-material meshes rendered mono-material.
It now resolves each range's own material (falling back to the item material).

Verified headless by `tools/import_test.cpp` (bgfx Noop backend); the cooked
skinned path (zombie) and all other regressions stayed green.
