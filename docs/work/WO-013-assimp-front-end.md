---
status: plan
id: WO-013
title: Assimp front end (static and skinned), retiring cookStatic/cookSkinned
program: assets
priority: P2
size: M
state: todo
depends: [WO-011]
contracts: [import-frontend]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
After this, `aiScene` never leaves the front end. The whole cook stack past the front end is Assimp-free.

## Done when
- [ ] the Assimp front end passes the WO-010 contract suite, skinned cases included
- [ ] `cookStatic` and `cookSkinned` are deleted
- [ ] cooked output matches the old `cookStatic`/`cookSkinned` for the FBX/OBJ/DAE test assets **except** the differences expected from `imported-scene.md` §7.1: textures become siblings, skinned tangent `w` is kept, rigid meshes in skinned files are kept, and mirrored instances are flipped
- [ ] **prove or refute, with a fixture first**: a cooked static FBX with an external texture renders untextured today, because its bare basename cannot resolve from the cooked directory (§7.1). This decides whether the switch is a fix or merely a change.
- [ ] the front end reproduces what the old Assimp paths did *before* the back end: one `PRESERVE_PIVOTS=false` configuration (the static path used the default, true, so compare static assets carefully); `ImproveCacheLocality` kept; nodes emitted in depth-first pre-order, the old emission order; the Mixamo junk-clip-name fallback to the file stem
- [ ] raw (uncompressed BGRA) embedded textures: `TextureRef::embedded` holds *encoded* bytes and no encoder is compiled in. Decide whether the type grows a raw-pixels form or the front end encodes.
- [ ] an audit rule: `aiScene`/`aiMesh`/`aiAnimation` may appear only in `src/assets/importers/` front-end files

## Contract
Nothing: the front end reports what Assimp read but the scene cannot hold, for example morph targets, in the dropped list.
