---
status: plan
id: WO-036
title: A real skinned glTF, cooked and animating in the editor
program: assets
priority: P2
size: S
state: todo
depends: [WO-014]
touches:
  - src/assets/import/frontend_cgltf.cpp
source: WO-014's last item, moved here on 2026-09-30 because no skinned glTF exists in the tree
---
## Why
WO-014 reads glTF skins and clips, and it is proven on glTF files the tests write, which pass the whole contract suite. But no skinned or animated glTF exists anywhere in the repository or in fps_shooter; every `.glb`/`.gltf` checked had zero skins and zero animations. A file written by a real exporter is the check that remains.

## Done when
- [ ] a real skinned, animated glTF is in the tree: a Khronos sample such as `CesiumMan.glb` or `Fox.glb` (CC-BY; downloaded with the user's permission), or one exported by the user
- [ ] it cooks as `MeshAsset` v6 with its bones and clips, and the cook log lists every loss
- [ ] it animates correctly in the editor, checked by eye by the user
- [ ] if it is small enough and its licence allows, it becomes a `cooker_test` fixture, so the path stays covered by a real file
