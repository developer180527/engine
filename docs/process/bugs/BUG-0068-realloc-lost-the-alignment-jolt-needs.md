## BUG-0068 — mem::realloc lost the alignment Jolt needs, and SSE code segfaulted on x86-64
- found:     2026-09-30
- status:    fixed
- class:     memory
- where:     src/core/memory/mem.cpp, src/plugins/jolt_plugin.h
- symptom:   determinism_gate and sim_replay segfaulted on the Linux x64 CI legs; UBSan on macOS reported `JPH::CharacterVirtual::Contact` constructed at a misaligned address. arm64 ran clean.
- cause:     `mem::realloc` reallocated at 8-byte alignment whatever the block had been allocated with. Jolt grows arrays of 16-byte-aligned types (`CharacterVirtual::Contact`) through `JPH::Reallocate`, so a grown array landed on an 8-byte boundary. arm64 tolerates misaligned vector loads; x86-64 SSE's aligned loads fault. Hidden until every CI leg could build (WO-038), because the x64 legs had been red at link time since 2026-09-07.
- pinned-by: tests/mem_test.cpp
- lane:      unit
- proof:     `mem_test` reallocates across many sizes (whether a TLSF block happens to be aligned depends on placement) and requires `alignof(max_align_t)` by default, 16 and 64 when asked, and a shrink to keep it. With the old 8-byte behaviour put back, 11 of the 16-byte and 25 of the 64-byte reallocs are misaligned, and the shrink check fails: red. Fixed in `0073ca0` (WO-038): `realloc` keeps `alignof(max_align_t)`, as `std::realloc` does, with an overload taking the block's alignment, and Jolt's hook passes 16. The Jolt tests are also clean under ASan+UBSan with halt_on_error.
