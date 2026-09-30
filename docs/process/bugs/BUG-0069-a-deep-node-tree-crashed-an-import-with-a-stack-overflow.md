## BUG-0069 — A deep node tree crashed an import with a stack overflow
- found:     2026-09-30
- status:    fixed
- class:     parse
- where:     src/assets/import/frontend_cgltf.cpp, src/assets/import/frontend_assimp.cpp, src/assets/import/frontend_assimp_skeleton.h
- symptom:   a glTF that is one chain of 2,000 nodes (44 KB) killed the importing thread with SIGBUS on a 512 KB stack, the size of a macOS secondary thread, where the job pool and the uncooked preview run. A 10,000-deep COLLADA crashed Assimp's own reader even on an 8 MB stack. WO-037 had closed with "nothing in the front ends may crash the cook worker", enforced by an exception boundary, and a stack overflow is not an exception.
- cause:     both front ends, and the Assimp skeleton extraction, walked the file's node tree by recursion, one frame per level: depth was an input nobody bounded. The fuzzer never built a tree deep enough. Separately, several walks were O(n x depth) (ancestor walks per joint, a subtree scan per node, worldOf per node), so a deep file could also stall a cook.
- pinned-by: tests/frontend_cgltf_test.cpp, tests/frontend_assimp_test.cpp
- lane:      unit
- proof:     each test imports a 10,000-deep chain above the triangle and above the skinned column, on a thread with a 512 KB stack (`engine::threads::runWithStack`); each crashed with SIGBUS (exit 138) before the fix. The Assimp test crashes again with the large import stack removed. `fuzz_import_frontend_test` generator 3 hangs one glTF case in ten under a chain up to 20,000 deep. Fixed in `d1cc5dc` (WO-039): explicit-stack walks in the recursive order (the new `extractSkeleton` is identical to the old one on every rigged FBX on disk), linear walks (`imp::worldsOf`), and Assimp's own recursion run on a 256 MB reserved stack; deeper still, the isolated `engine_cook_worker` is what dies.
