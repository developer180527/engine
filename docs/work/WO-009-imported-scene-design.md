---
status: plan
id: WO-009
title: ImportedScene — design the engine's own import format
program: assets
priority: P1
size: M
state: todo
new-contracts: [import-frontend]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
  - src/animation/clip_library.h
  - src/runtime/services/async_loader/parse.cpp
source: review 2026-09-29 C1, C3, C5
---
## Why
Nothing the engine owns sits between "a parser read the file" and "write the cooked asset", so every source format is a complete cook path of its own.

Assimp and cgltf each write `MeshAsset` directly, with vertex baking, materials and textures written twice. Animation takes `aiScene*`. Four parsers are kept in step only by comments. A new format means a third full copy.

**Design only.** It is a P1 because WO-010 to WO-018 all depend on it, and the design is cheap next to getting it wrong.

## Done when
- [ ] `docs/plans/imported-scene.md`: meshes (vertex streams, submeshes), materials with texture references, node hierarchy, skeleton, clips, and unit/axis conventions, which are converted once in the front end and never downstream
- [ ] **the "dropped" list is decided**: every front end reports what in the source it could not represent (the WO-002 lesson made general). A back end refuses a scene whose dropped list contains something the target needs.
- [ ] static and skinned meshes are decided: one type or two. This is the same question as retained-scene open question #13, so decide it once.
- [ ] `import-frontend` registered in `docs/contracts/` as `state: planned`, with all five meaning sections written
- [ ] the mapping from the four current parsers to front ends is listed, with what each stops doing

## Contract
`import-frontend`: `ImportResult importScene(path, options)` returns either an `ImportedScene` plus its `dropped` list, or an error.

Nothing: **stub** for a format with no front end: `Unsupported{extension}`, logged once. Never an empty scene: an empty scene is indistinguishable from an empty file.

Timing: synchronous. Imports run only inside cook workers, and the runtime never imports (WO-018), so no job handle is needed. That's decided here on purpose: an async import API would invite the runtime to call it.

Fake: a front end that builds a scene in memory from a description, so WO-011's back end is tested before any real parser is ported.

## Not in scope
USD, the FBX SDK, or any new format. The point is that each becomes one front end later.
