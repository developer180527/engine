---
status: plan
id: WO-037
title: Fuzz the import front ends; nothing in them may crash the cook worker
program: assets
priority: P1
size: S
state: done
done: 2026-09-30
evidence: fuzz_import_frontend_test (regress + explore lanes); frontend_cgltf_test §4 (5 cases) and import_frontend_contract_test §7 each red with their fix removed; 20,000 explore iterations clean, and under ASan+UBSan
contracts: [import-frontend]
touches:
  - src/assets/import/frontend_cgltf.cpp
  - src/assets/import/frontend_assimp.cpp
  - src/assets/import/import_frontend.h
  - src/assets/import/imported_scene_check.h
  - tests/fuzz_import_frontend_test.cpp
source: review 2026-09-30 of WO-013/014/036 ("is all production grade?")
---
## Why
The import front ends are the first code to read a model file from outside the engine, and fuzzing stopped at the cooked format.

A review found, by reading the code, that a skin with `"joints": []` crashed the cook worker. Once written, the fuzz target found a heap overflow the review had missed.

## Done when
- [x] a skin with no joints: the mesh is read static and dropped as Skin/Wrong, not a crash
- [x] every front end's `importScene` runs inside `guardedImport`: an exception refuses the file (`Unreadable`), never terminates the worker
- [x] `fuzz_import_frontend_test`: mutated glTF (JSON-tree and byte edits) and OBJ, built from the contract suite's reference scenes; no crash, no escaping exception, nothing invalid cooks, every cook loads back; registered in the fuzz-regress and fuzz-explore lanes with a seeded corpus
- [x] what the fuzzer found is fixed and pinned by a direct test: inverse-bind matrices shorter than the joint list (heap overflow, ASan) and accessors off their component alignment (UB, UBSan)
- [x] rotation keys are normalised on read, and `checkScene` refuses a non-unit rotation or a non-finite translation/scale key

## Contract
Tightens `import-frontend`'s **Errors**: "never thrown across the boundary" is enforced by `guardedImport`, and the front end checks what cgltf's validator does not.

Nothing: a file that trips a guard is refused as `Unreadable`, or cooked-and-refused through a `Wrong` drop, with a message naming the problem. No caller changes.

## Not in scope
- Fuzzing Assimp's own parsers beyond OBJ; Assimp is fuzzed upstream (OSS-Fuzz). OBJ covers the front end's own reading of Assimp's output.
- A coverage-guided (libFuzzer) lane; the harness is seeded and portable by design (tests/fuzz/fuzz.h).
