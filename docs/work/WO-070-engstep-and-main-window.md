---
status: plan
id: WO-070
title: "`engStep` and main-window control on today's internals — the C API's first, usable step"
program: providers
priority: P1
rank: 1
size: M
state: todo
depends: []
touches:
  - src/runtime/runtime_frame.cpp
  - src/runtime/runtime.h
  - src/runtime/platform/platform.h
source: docs/plans/engine-api.md §9 step 2; roadmap §2.8; DR-0028, DR-0030
---
## Why
The C API (DR-0028) is a 102-function draft that nothing implements; its frame and window model is the part every other piece stands on, and it can be proven on today's internals before anything else moves.

Today the frame is `EngineRuntime::frameBegin/tick/frameEnd`, the runtime pumps OS events itself and waits while minimised (`runtime_frame.cpp`), and a game cannot set its window's mode, monitor or close behaviour. DR-0030 says the host owns the loop: one call does one frame and returns.

## Done when
- [ ] `engStep` (and `engPumpEvents`, `engRun`) implemented over `EngineRuntime`, in a real header outside `docs/` but marked unstable (0.x), exporting only `eng*` symbols
- [ ] `ENG_PACING_HOST`: `engStep` never sleeps and never pumps events; `ENG_PACING_ENGINE`: today's behaviour, and where WO-061's limiter will sit
- [ ] main-window control from the draft's `eng_window.h`: mode (windowed, borderless, exclusive), size limits, monitor choice and listing, position, a close request the game may decline, and `ENG_WINDOW_NO_USER_MODE_CHANGE`
- [ ] `engine_player` runs on `engRun`; a test drives the headless platform through `engStep` alone for N frames and checks the simulation ticked exactly as `EngineRuntime::run` does today (same tick count, same replay hash)
- [ ] the draft's §10 checks run in CI on this header: C99 compile, no implicit padding, exported symbol list is `eng*` only
- [ ] what the implementation taught is written back into `engine-api.md` (anything in the draft that could not be built as written)

## Contract
Nothing: `EngineRuntime` and its hosts keep working unchanged; the C entry points are added beside them.

## Not in scope
Several windows and views (a later step of `engine-api.md` §9); the world half (WO-073).
