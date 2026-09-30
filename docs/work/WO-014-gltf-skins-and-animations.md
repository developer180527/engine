---
status: plan
id: WO-014
title: glTF skins and animations, supported for real
program: assets
priority: P2
size: M
state: todo
depends: [WO-012]
contracts: [import-frontend]
touches:
  - src/assets/importers/gltf_importer.cpp
source: review 2026-09-29 C2
---
## Why
This closes C2 properly. glTF is the format the engine says it owns, and today it can't carry a character.

## Done when
- [ ] the cgltf front end fills skeleton, skin weights and clips; the contract suite's skinned cases pass (`frontend_cgltf_test` skips `SkinnedColumn` and `AuthoredCentimetreZUp` today: remove the skip, and they must pass)
- [ ] a skinned `.glb` cooks as v3 and animates in the editor
- [ ] the WO-002 refusal is removed, and its test inverts: the same fixture now cooks with bones

## Contract
Nothing: an animation channel type the engine cannot play (for example morph weights) goes in the dropped list. The rest of the clip still cooks.
