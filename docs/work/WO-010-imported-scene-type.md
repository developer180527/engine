---
status: plan
id: WO-010
title: ImportedScene type, fake front end, and contract test
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: tests/import_frontend_contract_test.cpp (55 checks); LAYER-05; import-frontend provisional; mutations red
depends: [WO-009]
contracts: [import-frontend]
touches:
  - src/assets
source: review 2026-09-29 C1
---
## Why
Build the contract before either side of it, so the back end (WO-011) and the real front ends (WO-012, WO-013) can be built in any order, on any day.

## Done when
- [x] the `ImportedScene` header, with no Assimp, cgltf or GPU types in it (checked by an audit rule, in the same style as LAYER-04)
- [x] a fake front end that builds a scene in memory from a description
- [x] **one contract test suite** that every front end must pass: axis/units, the dropped list, submesh ranges, bone-weight normalisation. The fake passes it first.
- [x] `import-frontend` is `provisional` in the registry

## Contract
Nothing: the fake is the implementation until WO-012 and WO-013 land. Callers never depend on a real parser to compile or to test.

## Log
- 2026-09-30, `src/assets/import/`:
  - `imported_scene.h`: the type. It is dependency-free, with its own POD
    math types, because `Vec3` is `bx::Vec3`.
  - `import_frontend.h`: `IImportFrontend`, `ImportResult`, and
    `ImportFrontendRegistry`, whose no-match answer is the stub's
    Unsupported.
  - `imported_scene_check.h`: the structural invariants.
  - `fake_frontend.h`: the fake.
- **The contract suite is `tests/import_contract.h`: seven reference cases,
  compared by meaning.** It compares world-space triangles canonicalised by
  rotating corners, never reordering them, so winding survives. It compares
  the skeleton by bone name, clips by first and last key, and the dropped
  list as a multiset. So welding, reordering and node flattening are free for
  a real parser, and winding, units, axes, weights and losses are not.
- **Proven able to fail, not just shown passing.**
  - Section 3 breaks a scene 28 ways, and each `checkScene` check catches its
    own.
  - Section 5 runs the suite against 14 front ends broken the ways real
    parsers break: flipped winding, bottom-left UVs, ignored node transforms,
    collapsed instancing, merged submeshes, unconverted centimetres or Z-up,
    weights on the wrong bone, a re-parented bone, a wrong clip key, an
    unreported loss, the wrong loss effect, an empty scene instead of Empty,
    and a missing file that "succeeds". It names the case every time.
  - Section 6 shows a case a format cannot express is skipped, not passed.
- LAYER-05 is red for `<cgltf.h>`, `"render/mesh.h"` and `<bx/math.h>` in
  the format, and allows a `frontend_*` file its library. Disabling the
  weight-sum check reddens exactly its one case.
- Decided here: **morph targets are `Less`** (plan Q3). **FBX units on the
  skinned path (Q1) now has its fixture**, the `AuthoredCentimetreZUp` case.
  The fake passes it by construction, which proves only that the suite runs;
  the Assimp front end (WO-013) is what it will judge.
- Found on the way: the opaque pass sets both cull bits, which is undefined
  behaviour on D3D11 and Vulkan. Filed as WO-032, not fixed here.
