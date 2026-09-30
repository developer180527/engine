## BUG-0067 — A skinned mesh was bounded in bind space, so it spawned floating and culled wrong
- found:     2026-09-30
- status:    fixed
- class:     logic
- where:     src/assets/cookers/mesh/mesh_backend.cpp
- symptom:   once CesiumMan drew skinned (BUG-0065), he floated 0.76 m above the ground on spawn. The cooked bounds were a box on its side: (-0.13, -0.57, 0) to (0.18, 0.57, 1.51), a person lying along Z.
- cause:     the back end bounded a skinned mesh by its raw vertex positions, which are in BIND space, not where the mesh draws: the palette (bone rest world x inverse bind) moves them. For a Z-up bind space the box is on its side. The renderer culls with those bounds, and the editor's spawn scales and grounds the mesh by them.
- pinned-by: tests/real_gltf_test.cpp
- lane:      unit
- proof:     `real_gltf_test` requires the cooked bounds of Khronos CesiumMan to equal his skinned-at-rest box, feet at y = 0, which it computes independently from the file with cgltf's own helpers. With the bind-space bounds put back, feet are at -0.569 and it is red. Fixed in `103c66f` (WO-036): a skinned mesh is bounded skinned at rest, the same sum the contract suite's "skinned at rest" check uses (cooker v19).
