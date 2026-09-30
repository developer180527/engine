---
status: plan
id: WO-012
title: cgltf front end (static), retiring cookGltf
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
The first real front end. After this, glTF has no cook path of its own.

## Done when
- [ ] a cgltf front end passes the WO-010 contract suite
- [ ] `cookGltf` is deleted; `.gltf`/`.glb` go through the front end and the WO-011 back end
- [ ] cooked output is byte-identical to before for every glTF test asset (or each difference is explained and accepted)
- [ ] the WO-002 refusal moves into the front end's dropped list: skins and animations are reported, not silently skipped

## Contract
Nothing: a skinned glTF is still refused (dropped: skins), exactly as in WO-002, until WO-014.
