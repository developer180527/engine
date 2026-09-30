---
status: plan
id: WO-010
title: ImportedScene type, fake front end, and contract test
program: assets
priority: P2
size: M
state: todo
depends: [WO-009]
contracts: [import-frontend]
touches:
  - src/assets
source: review 2026-09-29 C1
---
## Why
Build the contract before either side of it, so the back end (WO-011) and the real front ends (WO-012, WO-013) can be built in any order, on any day.

## Done when
- [ ] the `ImportedScene` header, with no Assimp, cgltf or GPU types in it (checked by an audit rule, in the same style as LAYER-04)
- [ ] a fake front end that builds a scene in memory from a description
- [ ] **one contract test suite** that every front end must pass: axis/units, the dropped list, submesh ranges, bone-weight normalisation. The fake passes it first.
- [ ] `import-frontend` is `provisional` in the registry

## Contract
Nothing: the fake is the implementation until WO-012 and WO-013 land. Callers never depend on a real parser to compile or to test.
