---
status: plan
id: WO-021
title: "`os::` layer in core — one file per OS family"
program: portability
priority: P2
size: L
state: todo
depends: [WO-006, WO-005]
new-contracts: [os-services]
touches:
  - src/core
  - src/editor/panels/asset_browser/actions.h
  - src/tools
source: review 2026-09-29 P2, P4, P5
---
## Why
OS calls are scattered across memory, threads, module loading, process spawning, paths and editor tools.

A port should be one directory: `src/core/os/<family>/`.

## Done when
- [ ] `os::` covers: memory reserve/commit/release and aligned allocation, thread priority, dynamic libraries, process spawning, executable path and user directories, reveal in file manager
- [ ] posix, apple and win32 implementations; `mem.cpp`, `thread_qos.cpp`, `module_loader.h`, the cook workers and `engine_build` call `os::` only
- [ ] one contract test suite that runs on every CI platform
- [ ] OS-01's scope widens from `src/core` + `src/runtime` to all of `src/` (minus the vendored `src/editor/imgui/` backend) with zero findings. On 2026-09-30 that meant 22 bare `#else`s in 11 files: `shaderc_invoke.cpp` ×3, `engine_cook_worker.cpp` ×3, `miniaudio_provider.cpp`, `asset_browser/actions.h`, `terminal/model.h`, `project_context.h`, `engine_build.cpp`, plus `shader_blobs.h`, `device.cpp` and `output.cpp`, which belong to WO-024 (backend choice)
- [ ] cooking is written down as desktop-only (P4), as a decision, not an accident

## Contract
Nothing: **stub** per function on an OS without it: returns `NotSupported`, logged once per function. Examples: dynamic libraries on a console, reveal in file manager on a server.

**null** for the headless server: `revealInFileManager` is a no-op returning false.

No function silently succeeds.
