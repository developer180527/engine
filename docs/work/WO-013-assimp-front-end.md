---
status: plan
id: WO-013
title: Assimp front end (static and skinned), retiring cookStatic/cookSkinned
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: frontend_assimp_test (contract suite on COLLADA: 7 pass, 1 skipped with reason); cooked_texture_resolution_test (red on the old path, BUG-0063/0064); old-vs-new on 11 real files (imported-scene.md §7.3); IMP-01; mutations red
depends: [WO-011]
contracts: [import-frontend]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
After this, `aiScene` never leaves the front end. The whole cook stack past the front end is Assimp-free.

## Done when
- [x] the Assimp front end passes the WO-010 contract suite, skinned cases included (`Unrepresentable` is skipped with its reason: the vendored Assimp cannot read a COLLADA `<morph>`, and colours and cameras are checked on their own)
- [x] `cookStatic` and `cookSkinned` are deleted
- [x] cooked output matches the old `cookStatic`/`cookSkinned` for the FBX/OBJ/DAE test assets **except** the differences expected from `imported-scene.md` §7.1: textures become siblings, skinned tangent `w` is kept, rigid meshes in skinned files are kept, and mirrored instances are flipped
- [x] **prove or refute, with a fixture first**: a cooked static FBX with an external texture renders untextured today, because its bare basename cannot resolve from the cooked directory (§7.1). This decides whether the switch is a fix or merely a change.
- [x] the front end reproduces what the old Assimp paths did *before* the back end: one `PRESERVE_PIVOTS=false` configuration (the static path used the default, true, so compare static assets carefully); `ImproveCacheLocality` kept; nodes emitted in depth-first pre-order, the old emission order; the Mixamo junk-clip-name fallback to the file stem
- [x] raw (uncompressed BGRA) embedded textures: **decided, `TextureRef` grows a decoded-pixels form** (`rgba`, `width`, `height`); `checkScene` requires exactly one form and a size that fits
- [x] an audit rule: **IMP-01**, Assimp is included only by `src/assets/import/frontend_assimp.*` (the order named `importers/`, but the front ends live in `import/`); the five remaining users are baselined, each with the order that removes it

## Contract
Nothing: the front end reports what Assimp read but the scene cannot hold, for example morph targets, in the dropped list.

## Log
- 2026-09-30, **the texture suspicion was proven first**, as the order asked.
  `cooked_texture_resolution_test` (real `MeshCooker`, real `AssetService` on
  bgfx Noop) was red on the old path, and showed **two stacked bugs**:
  - **BUG-0063**: a bare basename that cannot resolve from the cooked
    directory
  - **BUG-0064**: `AssetService` binding a single-submesh mesh to the file's
    material 0, which for OBJ is Assimp's untextured default

  The test needed both fixes to go green.
- `imp::AssimpFrontend` reproduces the old paths' pre-cook behaviour: the same
  flags, `PRESERVE_PIVOTS=false`, depth-first nodes, the same skeleton and
  weight extractors, and the Mixamo name fallback. The Assimp memory gate
  moved into it. `mesh_cooker.cpp` is now 76 lines of dispatch. **Cooker
  version 18**: WO-012 changed glTF output without a bump, so glTF cooked in
  between would never have re-cooked; this bump covers both.
- **Compared before deleting, on eleven real files** (full table in
  `imported-scene.md` §7.3). The plants are byte-identical. Skinned models
  have identical skeletons, clips and weights. Every other difference is
  explained, and most are fixes:
  - 139 cannon vertices with no bone influence now follow the root bone
    instead of collapsing to the origin
  - 3 zero-length cottage tangents now have a valid fallback
  - three models' textures are now found
- **The contract suite found three things by running on real COLLADA:**
  - Assimp invents a "skeleton mesh" for files with no mesh. It is now off
    (`AI_CONFIG_IMPORT_NO_SKELETON_MESHES`).
  - The vendored Assimp cannot read a COLLADA `<morph>` at all.
  - Assimp names clips from the enclosing `<animation>`.
- **Two contract rules were wrong for real data and were corrected:**
  `inverseBind` need only be invertible, and the suite compares skeletons
  allowing extra ancestor bones and clips by the motion they cause.
- Mutations, each red on its own check:
  - skeleton meshes invented again
  - junk clip names kept
  - weights not read
  - no basename fallback
  - node transforms ignored
  - the BUG-0064 line reverted
- Filed WO-035 (FBX units: Assimp leaves FBX in centimetres; converting would
  rescale every project).
