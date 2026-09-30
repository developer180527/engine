---
status: plan
id: WO-041
title: Ledger the four bugs fixed on 2026-09-30 that have no entry
program: process
priority: P1
size: S
state: todo
touches:
  - docs/process/bugs/
source: engine audit 2026-09-30 (the ledger ends at BUG-0064; four later fixes are recorded only in work-order logs)
---
## Why
The bug ledger is how a class of bug is recognised the second time. Four real defects were found and fixed after BUG-0064, and each is written up only in a work order's log, which nobody searches by symptom:

- **every textured skinned mesh drew unskinned** (`opaque_pass.cpp`, since `1f0dff2`): the material's program replaced the skinning one. Found in WO-036; pinned by `render_pipeline_test` (`skinnedProgramDraws`).
- **`mem::realloc` lost alignment** (8 bytes): Jolt's 16-byte types segfaulted in SSE code on x86-64. Found in WO-038; pinned by `mem_test`.
- **a deep node tree crashed an import** with a stack overflow (44 KB of glTF). Found in WO-039's review; pinned by `frontend_{cgltf,assimp}_test` §4.
- **a rig over 128 bones was silently truncated**, or cooked and never animated. Found in WO-039, fixed in WO-040; pinned by `mesh_backend_test`, `frontend_*_test` §5.

## Done when
- [ ] BUG-0065 to BUG-0068, one per defect above, in the ledger's format (found, status, class, where, symptom, cause, pinned-by, lane, proof)
- [ ] each names the commit that fixed it and the test that fails with the fix reverted
