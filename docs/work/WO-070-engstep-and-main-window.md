---
status: plan
id: WO-070
title: "`engStep` and main-window control on today's internals — the C API's first, usable step"
program: providers
priority: P1
rank: 1
size: M
state: active
depends: []
touches:
  - src/runtime/runtime_frame.cpp
  - src/runtime/runtime.h
  - src/runtime/platform/platform.h
  - src/runtime/platform/glfw_platform.cpp
  - src/capi/eng_capi.cpp
  - include/eng/engine.h
  - src/tools/engine_player.cpp
source: docs/plans/engine-api.md §9 step 2; roadmap §2.8; DR-0028, DR-0030
---
## Why
The C API (DR-0028) is a 102-function draft that nothing implements; its frame and window model is the part every other piece stands on, and it can be proven on today's internals before anything else moves.

Today the frame is `EngineRuntime::frameBegin/tick/frameEnd`, the runtime pumps OS events itself and waits while minimised (`runtime_frame.cpp`), and a game cannot set its window's mode, monitor or close behaviour. DR-0030 says the host owns the loop: one call does one frame and returns.

## Done when
- [x] `engStep` (and `engPumpEvents`, `engRun`) implemented over `EngineRuntime`, in a real header outside `docs/` but marked unstable (0.x), exporting only `eng*` symbols
- [x] `ENG_PACING_HOST`: `engStep` never sleeps and never pumps events; `ENG_PACING_ENGINE`: today's behaviour, and where WO-061's limiter will sit
- [x] main-window control from the draft's `eng_window.h`: mode (windowed, borderless, exclusive), size limits, monitor choice and listing, position, a close request the game may decline, and `ENG_WINDOW_NO_USER_MODE_CHANGE`
- [x] `engine_player` runs on `engRun`; a test drives the headless platform through `engStep` alone for N frames and checks the simulation ticked exactly as `EngineRuntime::run` does today (same tick count, same replay hash)
- [x] the draft's §10 checks run in CI on this header: C99 compile, no implicit padding, exported symbol list is `eng*` only
- [x] what the implementation taught is written back into `engine-api.md` (anything in the draft that could not be built as written)
- [ ] **checked by eye by the owner** (`build/eng_window_manual`): the window goes borderless fullscreen and back (and exclusive, where offered), the macOS green button does nothing, the first close is declined and the second accepted. These need a person; `eng_window_test` covers everything else

## Contract
Nothing: `EngineRuntime` and its hosts keep working unchanged; the C entry points are added beside them.

## Not in scope
Several windows and views (a later step of `engine-api.md` §9); the world half (WO-073).

## Log
- 2026-11-06:
  - **Built:** `include/eng/` (`eng_base.h`, `eng_engine.h`, `eng_window.h`, `engine.h`), 29 functions, only what is implemented; the rest stays in the draft. `src/capi/eng_capi.cpp` over `EngineRuntime`; every entry point catches every exception; every refusal names its reason (DR-0035). `engine_capi` (static) everywhere, `eng` (shared, `eng*` exports only) on macOS.
  - **The frame:** `frameBegin` split so `engStep` gets the housekeeping (profiler, frame arena, uploads, residency) without the pump and the minimised wait. A minimised window ticks the simulation and skips only drawing (`tickNoRender`, `frameEnd(false)`).
  - **The platform:** `IPlatform` gained main-window control with "unsupported" defaults; GLFW implements it (mode, monitors with stable ids, size, position, limits, aspect, flags, show, focus, window events by comparing state, a chained close callback that can decline). macOS: `platwin::setUserFullscreenEnabled` (collection behaviour) for `ENG_WINDOW_NO_USER_MODE_CHANGE`. SDL3 reports these capabilities as unavailable.
  - **Evidence:** `eng_step_test`: the same 13-body physics scene through `EngineRuntime`'s loop and through `engStepWithDelta`, 300 frames: 300 ticks each, world hash `e24fbefbf87167a5` both; `simulation_steps` sums to the ticks; 13 refusals by name; a struct written only up to its `struct_size`. `eng_window_test` (C, hidden window, skips without a display): monitors before a window, limits, a refused second window, RESIZED and MOVED events, limits holding against code, a stale handle after destroy. `eng_exports`: 29 declared, the same 29 exported, nothing else; every struct laid out by the padding check. `eng_header_check`: strict C99, `-Wpadded`, 12 structs, no padding. Mutations, each caught: a doubled step time (600 ticks against 300); a declared function the library lacks; a struct missing from the padding list; a padded struct.
  - **engine_player** runs on `engCreateEngine`, `engCreateWindow` and `engRun` (`--frames`/`--gpu-stats` on `engStep`): 120 frames of a cooked project, 3 draws steady; no project still exits 2, no cooked scene 3.
  - **Found on the way:** GLFW size limits do not bind sizes set from code (the platform clamps now); `ProjectContext::load` creates folders, so a project path is checked for existence first.
  - **Not done here, recorded in `engine-api.md` §10a:** the shared library on Linux (needs position-independent code); a window after a headless start; `interpolation_alpha`; monitor added/removed events.
  - Full unit lane: 106 of 106 after the one audit finding (a bare `#else`) was annotated.
