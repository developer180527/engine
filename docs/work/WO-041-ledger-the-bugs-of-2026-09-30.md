---
status: plan
id: WO-041
title: Ledger the bugs fixed on 2026-09-30 that have no entry
program: process
priority: P1
size: S
state: done
done: 2026-09-30
evidence: BUG-0065..0070 in docs/process/bugs/, all passing engine_doctor's ledger checks; each fix re-reverted and seen red (BUG-0066's pin is new: async_loader_test §3)
touches:
  - docs/process/bugs/
  - tests/async_loader_test.cpp
source: engine audit 2026-09-30 (the ledger ends at BUG-0064; four later fixes are recorded only in work-order logs)
---
## Why
The bug ledger is how a class of bug is recognised the second time. Four real defects were found and fixed after BUG-0064, and each is written up only in a work order's log, which nobody searches by symptom:

- **every textured skinned mesh drew unskinned** (`opaque_pass.cpp`, since `1f0dff2`): the material's program replaced the skinning one. Found in WO-036; pinned by `render_pipeline_test` (`skinnedProgramDraws`).
- **`mem::realloc` lost alignment** (8 bytes): Jolt's 16-byte types segfaulted in SSE code on x86-64. Found in WO-038; pinned by `mem_test`.
- **a deep node tree crashed an import** with a stack overflow (44 KB of glTF). Found in WO-039's review; pinned by `frontend_{cgltf,assimp}_test` §4.
- **a rig over 128 bones was silently truncated**, or cooked and never animated. Found in WO-039, fixed in WO-040; pinned by `mesh_backend_test`, `frontend_*_test` §5.

## Done when
- [x] one entry per defect, in the ledger's format (found, status, class, where, symptom, cause, pinned-by, lane, proof): BUG-0065 to BUG-0070, six, not four (see the log)
- [x] each names the commit that fixed it and the test that fails with the fix reverted

## Log
- 2026-09-30: **Six entries, not four.** The audit counted one bug per work
  order. WO-036's commit fixed three: skinned meshes drawn unskinned
  (BUG-0065), a cooked glTF spawned without its skeleton (BUG-0066), and
  skinned bounds taken in bind space (BUG-0067). BUG-0068 is `realloc`'s
  alignment, BUG-0069 the deep-tree crash, BUG-0070 the bone limit.
- **Every proof was re-run for this ledger, not copied from a log.** Each
  fix was reverted and its test seen red:
  - BUG-0065: `skinnedProgramDraws` 0 of 4
  - BUG-0067: feet at -0.569
  - BUG-0068: 11 and 25 misaligned, matching `0073ca0`'s own numbers
  - BUG-0069: SIGBUS 138
  - BUG-0070: all four mutations
- **BUG-0066 had no pin, so it got one.** The spawn-path fix was never
  tested. `async_loader_test` §3 now cooks a skinned glTF through the real
  pipeline and requires `hasCooked()` false before the cook and true
  after, and the cooked load to return the skeleton and the clip. It is
  red with `hasCooked()` stubbed. `spawnFile`'s own branch needs ImGui and
  stays untested; WO-018 removes the branch it chooses between.
- **Test headers.** `gltf_writer.h` and `import_contract.h` now pin every
  ImportedScene type they use (`using imp::Mesh;` and the rest): the
  renderer's `Mesh` and `Material` collide as the animation types did.
