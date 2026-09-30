---
status: plan
id: WO-009
title: ImportedScene — design the engine's own import format
program: assets
priority: P1
size: M
state: done
done: 2026-09-30
evidence: docs/plans/imported-scene.md; docs/contracts/import-frontend.md (planned, five sections written)
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
- [x] `docs/plans/imported-scene.md`: meshes (vertex streams, submeshes), materials with texture references, node hierarchy, skeleton, clips, and unit/axis conventions, which are converted once in the front end and never downstream
- [x] **the "dropped" list is decided**: every front end reports what in the source it could not represent (the WO-002 lesson made general). A back end refuses a scene whose dropped list contains something the target needs.
- [x] static and skinned meshes are decided: one type or two. This is the same question as retained-scene open question #13, so decide it once.
- [x] `import-frontend` registered in `docs/contracts/` as `state: planned`, with all five meaning sections written
- [x] the mapping from the four current parsers to front ends is listed, with what each stops doing

## Contract
`import-frontend`: `ImportResult importScene(path, options)` returns either an `ImportedScene` plus its `dropped` list, or an error.

Nothing: **stub** for a format with no front end: `Unsupported{extension}`, logged once. Never an empty scene: an empty scene is indistinguishable from an empty file.

Timing: synchronous. Imports run only inside cook workers, and the runtime never imports (WO-018), so no job handle is needed. That's decided here on purpose: an async import API would invite the runtime to call it.

Fake: a front end that builds a scene in memory from a description, so WO-011's back end is tested before any real parser is ported.

## Not in scope
USD, the FBX SDK, or any new format. The point is that each becomes one front end later.

## Log
- 2026-09-30: Design written from a reading of all seven read sites, not from
  the review. Measured: two libraries, seven read sites, five different Assimp
  configurations (`PRESERVE_PIVOTS` true in two and false in three;
  `ImproveCacheLocality` in the cooker only), and two texture-lookup rules.
  The cooker stores a basename, while `parse.cpp` searches four directories
  and matches embedded names case-insensitively.
- Found by reading: **a glTF with no `TANGENT` gets a constant `(1,0,0,1)` on
  every vertex**, so a normal-mapped glTF exported without tangents shades
  wrong, while Assimp computes them. Tangent generation becomes a back-end job
  for every format. MikkTSpace is not in `third_party/`, so the generator is
  an open question for WO-011.
- **Correction to this order and WO-020:** static-vs-skinned here is *not*
  retained-scene question #13. #13 is the render instance table's layout,
  decided at runtime; this is the import representation. The import question
  is decided (one mesh type); #13 stays open.
- The texture-lookup rule is deliberately stricter than `parse.cpp`: no
  directory search and no case-insensitive match. Some assets that work today
  by accident will report a missing texture instead.
- FBX units and axes on the skinned path are left as an open question with a
  required fixture, rather than asserted: the static path gets them right via
  baked node transforms, and the skinned path does not obviously do the same.
- Also corrected: my WO-002 comment called the skinned payload "v3", but
  `cookSkinned` writes `MeshAsset` header version 6.
