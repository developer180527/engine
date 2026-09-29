---
status: plan
id: WO-006
title: An unknown OS is a compile error with a to-do list, not silently POSIX
program: portability
priority: P1
size: M
state: todo
touches:
  - src/core/memory/mem.cpp
  - src/core/frame_arena.h
  - src/core/thread_qos.cpp
  - src/runtime/module_loader.h
  - scripts/engine_audit.py
source: review 2026-09-29 P2
---
## Why
Core memory, threading and module loading go `#if _WIN32 … #else POSIX`, so a new port compiles, then quietly does the wrong thing.

The compiler should hand a port its to-do list instead.

## Done when
- [ ] every `#else` after a `_WIN32` branch in `src/core` and `src/runtime` becomes `#elif defined(__APPLE__) || defined(__linux__)`, with `#else #error "port: implement <what>"`
- [ ] each `#error` names the functions the port must provide, and the list goes in `docs/process/porting.md` (generated from the `#error`s if cheap)
- [ ] audit rule **OS-01**: a bare `#else` after an OS check in `src/core`/`src/runtime` is a finding, with today's leftovers baselined
- [ ] the macOS, Linux and Windows builds are unchanged

## Not in scope
Moving the code behind `os::` (WO-021). This order makes the gaps loud; that one closes them.
