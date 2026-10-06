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
- [x] Sanitizers and Linux x64: `mem::realloc` reallocated at 8-byte alignment, and Jolt grows arrays of 16-byte `CharacterVirtual::Contact` through it. UBSan on macOS; a real segfault in SSE code on x86-64 (`determinism_gate`, `sim_replay`). `realloc` now keeps `max_align_t`, with an overload for more, and Jolt's hook passes 16.
- [ ] a `workflow_dispatch` run of the full matrix is green on all six legs plus the sanitizer, SDK-only and shipping jobs (Windows may show more once it links)
- [ ] a failed nightly notifies instead of sitting red for weeks

## Contract
Changes `kit-abi`'s build surface: `engine::headers` now carries `flecs_STATIC`, matching how the engine links flecs. A Windows module still needs the host to export flecs to run (WO-025). Nothing changes for Linux or macOS modules.

Nothing: until a leg is green, it says nothing about its platform. No platform is claimed as supported on the strength of a red leg.

## Not in scope
New platforms or architectures. They are worth adding only on top of a green baseline.

## Log
- 2026-09-30, first full-matrix run after the build fixes: every leg now builds, which it had not since September. It exposed what the build failures had hidden:
  - the Jolt misalignment (above) is a crash on x86-64, not only a sanitizer report;
  - `real_gltf_test` rounded positions to a 1 mm grid and compared cells, so sums a hair apart straddled cell edges on Linux (FMA contraction differs from Apple clang). It now matches within 1 mm, both ways; still red when the inverse-bind matrices are skipped.
  - `engine_module_probe` did not export its symbols (`ENABLE_EXPORTS`), unlike every other host, so on Linux every module, the ABI fixtures and any user's kit, failed with "undefined symbol: EcsOnLoad" and was reported as refused.
  - `work_orders_test` held `brief` to 2 s of wall clock on a cold CI runner (3.15 s there, 0.1 s locally). `brief` now skips dirty submodules; the budget applies off CI.
  - `seam_cost_bench` used `<dlfcn.h>` directly (Windows compile error). It has a per-OS loader, its plugin exports `seamDraw` on Windows, and a plugin that fails to load now fails the benchmark instead of reporting success.
- 2026-10-03, second full run: macOS, Linux arm64 and both sanitizer jobs green; Linux x64 and Windows still red, plus three new Windows failures from the WO-015..050 commits. Fixed, not yet confirmed by a run:
  - Linux x64: `applyCubeLut` read `(int)NaN` (INT_MIN on x86-64, 0 on arm64) as a table index. A NaN channel now reads as 0; UBSan reports the old code on any CPU. The sanitizer job ran the unit lane only, so it now runs fuzz-regress as well.
  - Windows `std::system`: cmd.exe strips the first and last quote of a line that starts with one, so every tool a test launched by quoted path failed to start (`sim_replay`, `sdk_only_game`, and probably the `addon_protocol` hang). `tests/shell_run.h` adds the outer quotes and returns the real exit code.
  - Windows stdout is text mode: `writeManifest` now writes the framed bytes in binary, so `\n` stays `\n` and the digest matches (`module_abi_conformance`).
  - the terminal panel: the project root was unquoted (every OS) and `cd` without `/d` cannot change drive on Windows; its test now ignores the trailing space cmd's `echo` keeps.
  - `async_loader_test`, `clip_cook_test`: passed every check, then `fs::remove_all` threw on the still-open `registry.db` (0xc0000409). The registry is closed first and cleanup cannot fail a test.
  - `player_has_no_cook_stack`: MSVC keeps symbols in the PDB, so on x64 `nm` read an empty table and PASSED with nothing checked, and on arm64 it could not read the file. MSVC now writes a link map (`/MAP`) and the script reads it; no symbols to read is a failure.
  - `architecture_audit`: DOC-01 compared `\`-separated paths with `/` ones; `cooked_format_conformance`: a newline filename is not applicable on Windows rather than a skip.
  - still open: the `addon_protocol_test` hang is not diagnosed (the quote fix may cure it), and `audio_abi_conformance` failed once on macOS CI comparing the audio sample clock with the host clock under load (push run 36778999094) — a timing flake to look at.

- 2026-10-06, nightly 37438910117: every leg green except Windows, which now builds. Windows arm64 failed three tests (x64 hung in the same lane):
  - `sdk_only_game_test` and `addon_protocol_test` hung in the first `engine_cook` / `engine_build` call, even `engine_build` with no arguments. All four CLI tools began with `setvbuf(stdout, nullptr, _IOLBF, 0)`: a size of 0 is an invalid parameter to MSVC's CRT, and a Debug build answers it with an assertion dialog nobody closes. `src/tools/stdout_live.h` sets no buffering on Windows (MSVC has no line buffering) and `_IOLBF` with `BUFSIZ` elsewhere. This was the undiagnosed `addon_protocol_test` hang.
  - `player_has_no_cook_stack` matched the log channel name `"ShaderCook"` (`core/logger.h`): MSVC's link map spells a string literal with its text (`??_C@...`), which `nm` never lists. The check now skips literal records; a real `shadercook::` symbol still fails it.
