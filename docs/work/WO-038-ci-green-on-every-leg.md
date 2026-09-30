---
status: plan
id: WO-038
title: CI green on every leg, and red is noticed
program: portability
priority: P0
size: M
state: active
touches:
  - .github/workflows/ci.yml
  - src/CMakeLists.txt
  - tests/CMakeLists.txt
  - tests/perf/CMakeLists.txt
  - tests/fixtures/abi_gate_module.cpp
  - tests/thread_qos_test.cpp
source: review 2026-09-30 ("should we run CI on multiple platforms and CPU architectures?")
---
## Why
CI already builds six legs across three OSes and two architectures, and it has not been green since 2026-09-06, so it protects nothing.

A red CI teaches everyone to ignore it. WO-006's "confirmed by the next nightly" has never had a green nightly to confirm anything.

## Done when
- [x] Linux (all three legs): `hdr_surface_stub.cpp` linked on Linux/BSD. Colour stage C added it to the Apple and Windows branches only, so every Linux link failed on three `platwin::` symbols from 2026-09-17.
- [x] Linux GCC: `headless_include_probe` is an OBJECT library. Its property is "compiles", and as an executable linking nothing it failed on flecs.h's C++ constants from 2026-09-07.
- [x] Windows (both legs): `engine::headers` defines `flecs_STATIC`, so SDK modules stop declaring flecs as a DLL import; the ABI-gate fixtures link a private flecs on Windows (a DLL resolves at link time); the probe takes bx's compat directory per compiler, not `compat/osx`; the seam-cost benchmark sits in its own directory so MSVC Debug can drop `/RTC1`, which `/O2` refuses.
- [x] macOS: `thread_qos_test` waits, bounded, for a pool worker instead of assuming one wins a race against the calling thread (0 of 48 failed under full CPU contention).
- [ ] Sanitizers: `JPH::CharacterVirtual::Contact` constructed misaligned (16-byte type), found by UBSan. Investigate before fixing: probably the Jolt allocator hook's alignment.
- [ ] a `workflow_dispatch` run of the full matrix is green on all six legs plus the sanitizer, SDK-only and shipping jobs (Windows may show more once it links)
- [ ] a failed nightly notifies instead of sitting red for weeks

## Contract
Changes `kit-abi`'s build surface: `engine::headers` now carries `flecs_STATIC`, matching how the engine links flecs. A Windows module still needs the host to export flecs to run (WO-025). Nothing changes for Linux or macOS modules.

Nothing: until a leg is green, it says nothing about its platform. No platform is claimed as supported on the strength of a red leg.

## Not in scope
New platforms or architectures. They are worth adding only on top of a green baseline.
