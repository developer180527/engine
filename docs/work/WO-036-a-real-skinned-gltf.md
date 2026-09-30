---
status: plan
id: WO-036
title: A real skinned glTF, cooked and animating in the editor
program: assets
priority: P2
size: S
state: done
done: 2026-09-30
evidence: real_gltf_test (spec oracle, cook, bounds); render_pipeline_test skinnedProgramDraws; the user saw CesiumMan walk on the ground in the editor on 2026-09-30
depends: [WO-014]
touches:
  - tests/real_gltf_test.cpp
  - src/render/pipeline/opaque_pass.cpp
  - src/assets/cookers/mesh/mesh_backend.cpp
  - src/editor/panels/asset_browser/spawn.h
  - tests/fixtures/gltf/
source: WO-014's last item, moved here on 2026-09-30 because no skinned glTF exists in the tree
---
## Why
WO-014 reads glTF skins and clips, and it is proven on glTF files the tests write, which pass the whole contract suite. But no skinned or animated glTF exists anywhere in the repository or in fps_shooter; every `.glb`/`.gltf` checked had zero skins and zero animations. A file written by a real exporter is the check that remains.

## Done when
- [x] a real skinned, animated glTF is in the tree: `tests/fixtures/gltf/CesiumMan.glb` (Khronos sample, COLLADA2GLTF, © Cesium, CC-BY 4.0; downloaded with the user's permission on 2026-09-30; credited in `tests/fixtures/gltf/README.md`)
- [x] it cooks as `MeshAsset` v6 with its bones and clips, and the cook log lists every loss: 21 bones, 1 clip with all 19 tracks mapped, and nothing dropped (its embedded texture cooks too)
- [x] it animates correctly in the editor, checked by eye by the user on 2026-09-30: he walks in place, upright, textured with the Cesium logo the right way round
- [x] if it is small enough and its licence allows, it becomes a test fixture, so the path stays covered by a real file: `real_gltf_test`, 438 KB

## Log
- 2026-09-30: `real_gltf_test` reads CesiumMan with the cgltf front end and
  checks it against a second opinion, computed in the test straight from the
  spec using cgltf's own helpers (`cgltf_accessor_read_*`,
  `cgltf_node_transform_world`) and none of the front end's code:
  - **the file's facts:** 19 joints, 57 channels, and a 2 s clip, all read
  - **at rest:** every vertex, skinned by Σ weight × jointWorld × inverseBind,
    is where the reader's scene skins it (2338 places out of 14016 corners, 0
    off). He is 1.51 m tall along +Y with his feet at y = 0.
  - **the clip:** every joint's world origin, posed from the file's own
    samplers at the first and last key, is where the reader's clip puts it
  - **the cook:** MeshAsset v6, 21 bones (the joints plus the file's `Z_UP` and
    `Armature` nodes), and one clip with 19 of 19 tracks mapped

  The file has the shape the written fixtures don't: its conversion sits
  above the armature, and its mesh node is a sibling of the joints. The reader
  passed on the first run; no reader change was needed.
- **Why not Assimp as the second opinion.** This build compiles Assimp without
  its glTF importer (`CMakeLists.txt`, `ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT
  OFF`). Turning it on would ship a second glTF parser to the editor just to
  test the first, so the spec computed with cgltf's helpers is the oracle
  instead.
- Mutations, each red, with the source restored byte-exact:
  - inverse binds ignored: every vertex is off
  - ancestors dropped: 19 bones, all vertices off, 38 joints off
  - rotation keys dropped: 38 of 57 channels
  - quaternions conjugated: 36 joints off
- Left for the user: load `tests/fixtures/gltf/CesiumMan.glb` in the editor
  and watch the clip play.
- **The editor check found three bugs, none of them in the importer.** CesiumMan
  spawned lying down and never moved:
  1. **Spawn used the uncooked preview.** A dropped `.glb` always went through
     the runtime glTF importer, which reads static geometry only. Now a glTF
     with a Ready cooked version spawns through the async loader's cooked path,
     which carries the skeleton and clips (`AsyncLoader::hasCooked`).
  2. **Every textured skinned mesh drew unskinned** (`opaque_pass.cpp`, since
     `1f0dff2`, "materials become data"). Binding a material replaced the
     skinning program with the material's static one, so the palette was
     uploaded and ignored and the raw bind-space vertices were drawn. His are
     Z up. This affected every skinned character, FBX included. A skinned draw
     now keeps the skinning program. A new counter, `skinnedProgramDraws`,
     must equal `skinnedDraws`, and `render_pipeline_test` checks it: 0 of 4
     with the old line, 4 of 4 fixed.
  3. **Skinned bounds were taken in bind space.** The renderer culls with the
     bounds, and spawn scales and grounds by them, so he floated 0.76 m. The
     back end now bounds a skinned mesh skinned at rest (cooker v19).
     `real_gltf_test` checks the cooked box is his rest box with feet at y = 0:
     −0.569 with the old bounds, 0 fixed.

  A headless probe found the second bug. It went through the editor's own
  path: the registry, `AsyncLoader`, the spawn's components, and
  `AnimatorSystem`. The palette moved every frame, and the palette-skinned
  vertices stood up while the raw ones lay down, which pointed at the draw
  call. The probe is deleted.
- 2026-09-30: The user confirmed it in the editor: he walks, upright, feet on the ground. Closed.
