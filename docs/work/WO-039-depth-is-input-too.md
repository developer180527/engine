---
status: plan
id: WO-039
title: A deep node tree must not crash an import; depth is input too
program: assets
priority: P1
size: S
state: done
done: 2026-09-30
evidence: frontend_cgltf_test and frontend_assimp_test §4 (10,000-deep chains on a 512 KB stack, each SIGBUS before the fix); fuzz_import_frontend_test generator 3; extractSkeleton identical to the old one on 5 real rigs, helper chains included; import_frontend_contract_test's 0.6 mm case
contracts: [import-frontend]
touches:
  - src/assets/import/frontend_cgltf.cpp
  - src/assets/import/frontend_assimp.cpp
  - src/animation/assimp_skeleton_loader.h
  - src/assets/cookers/mesh/mesh_backend.cpp
  - src/core/thread_stack.cpp
  - tests/import_contract.h
source: review 2026-09-30 of WO-037/038 ("any issues? production grade code?")
---
## Why
WO-037 closed with "nothing in them may crash the cook worker", and it was
enforced with an exception boundary. A stack overflow is not an exception. Both
front ends walked the file's node tree by recursion, so a glTF that is one
chain of 2,000 nodes (44 KB) crashed with SIGBUS on a 512 KB stack: the size of a
macOS secondary thread, where the job pool and the uncooked preview import. The
fuzzer never built a tree that deep.

## Done when
- [x] the cgltf front end, the Assimp front end and `anim::extractSkeleton` walk node trees with explicit stacks, in the recursive order exactly, so cooked output is unchanged
- [x] the walks that were O(n x depth) are linear: the ancestor walks stop at a kept node, the skeleton's "is there a bone below?" is one pass, world matrices are computed once (`imp::worldsOf`), and the back end looks rigid meshes' bones up by name
- [x] Assimp's own recursion (its COLLADA reader overflowed even 8 MB at 10,000 levels, and `aiNode`'s destructor recurses too) runs on a 256 MB reserved stack (`engine::threads::runWithStack`); deeper still, the isolated cook worker is what dies
- [x] tests: a 10,000-deep chain above the triangle and the skinned column, both formats, on a 512 KB stack, each SIGBUS before; the fuzzer hangs one glTF case in ten under a chain up to 20,000 deep (generator 3)

## Log
- 2026-09-30: The rewrite of `collectSkeletonNodes` was checked against the
  old recursive one on every rigged model on disk: 5 FBX rigs, with and
  without FBX pivots (up to 115 `$AssimpFbx$` helper nodes). The bones,
  parents and matrices are identical.
- The same review found the contract suite pairing triangles by position
  rounded to a 1 mm grid, the flake WO-038 fixed in `real_gltf_test`.
  `compareGeometry` now pairs each triangle with one whose corners lie within
  tolerance under a rotation. A vertex 0.6 mm off, across a rounding edge,
  failed before and passes now; every broken-front-end case still fails.
- Fuzz cost, for budgets: about 0.25 s per iteration (the cook dominates), and
  the deep cases add about 15%.

## Contract
Tightens `import-frontend`'s **Errors**: depth is input, and no front end may
recurse on it.

Nothing: a file too deep even for the 256 MB import stack crashes the process
it runs in. For a cook, that is the isolated `engine_cook_worker`, and the
asset fails. In-process imports (`COOK_INPROC=1`, and the uncooked preview
WO-018 removes) have no such backstop.
