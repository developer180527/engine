---
status: plan
id: WO-011
title: One cook back end — ImportedScene to cooked assets
program: assets
priority: P2
size: L
state: todo
depends: [WO-010]
contracts: [cooker]
touches:
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1
---
## Why
Vertex baking, normal matrices, tangents, materials and texture resolution get written **once**, against `ImportedScene`, instead of once per parser.

## Done when
- [ ] back end turns an `ImportedScene` into `MeshAsset` v2 (static) and v3 (skinned)
- [ ] tested through the WO-010 fake front end only, with no real file parsed
- [ ] refuses a scene whose dropped list contains something the target format needs (WO-009)
- [ ] the cooked output is **byte-identical** to today's for the existing test assets once WO-012 and WO-013 land (checked there, recorded here as the bar)

## Contract
Nothing: while front ends are unported, the old per-parser paths stay live behind the extension dispatch. The back end is additive until WO-012 and WO-013 switch each format over.
