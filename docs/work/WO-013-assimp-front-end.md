---
status: plan
id: WO-013
title: Assimp front end (static and skinned), retiring cookStatic/cookSkinned
program: assets
priority: P2
size: M
state: todo
depends: [WO-011]
new-contracts: [import-frontend]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
After this, `aiScene` never leaves the front end. The whole cook stack past the front end is Assimp-free.

## Done when
- [ ] the Assimp front end passes the WO-010 contract suite, skinned cases included
- [ ] `cookStatic` and `cookSkinned` are deleted
- [ ] byte-identical cooked output for the FBX/OBJ/DAE test assets (or each difference is explained)
- [ ] an audit rule: `aiScene`/`aiMesh`/`aiAnimation` may appear only in `src/assets/importers/` front-end files

## Contract
Nothing: the front end reports what Assimp read but the scene cannot hold, for example morph targets, in the dropped list.
