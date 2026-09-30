---
status: plan
id: WO-006
title: An unknown OS is a compile error with a to-do list, not silently POSIX
program: portability
priority: P1
size: M
state: active
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
- [x] every `#else` after a `_WIN32` branch in `src/core` and `src/runtime` becomes `#elif defined(__APPLE__) || defined(__linux__)`, with `#else #error "port: implement <what>"`
- [x] each `#error` names the functions the port must provide, and the list goes in `docs/process/porting.md` (generated from the `#error`s if cheap)
- [x] audit rule **OS-01**: a bare `#else` after an OS check in `src/core`/`src/runtime` is a finding, with today's leftovers baselined
- [ ] the macOS, Linux and Windows builds are unchanged. macOS was built locally on 2026-09-30; Linux and Windows are confirmed by the next nightly CI (Linux compiles the new Wayland branch in `GlfwToolWindow` for the first time)

## Not in scope
Moving the code behind `os::` (WO-021). This order makes the gaps loud; that one closes them.

## Log
- 2026-09-30: `src/core/os_family.h` defines `ENGINE_OS_POSIX` once (Apple,
  Linux). Every POSIX-specific `#else` in `src/core` and `src/runtime` is now
  `#elif ENGINE_OS_POSIX … #else #error "port: …"`: `mem.cpp` ×5,
  `frame_arena.h` ×2, `module_loader.h` ×2. The genuine fallbacks carry
  `// any OS: <why>`: `thread_qos.cpp` ×2, `ui_input.h`, and both
  display-connection getters.
- Found by doing it:
  - **`GlfwToolWindow::nativeWindowHandle` sent Linux on Wayland to X11**
    while the main window, three functions up, returned a Wayland surface.
    It now follows the main window.
  - **Four native-window-handle getters returned `nullptr` on an unknown
    OS**: `window_ops_glfw`, `window_ops_sdl3`, and `sdl3_platform` ×2. That
    meant "nothing renders" and nothing said why. They are `#error` now,
    like the GLFW main window already was.
  - OS-01 itself found a site my first survey missed: `sdl3_platform.cpp:79`.
    The survey's pattern lacked `SDL_PLATFORM_*`.
- `docs/process/porting.md` is generated (`engine_audit.py --write-porting`,
  15 items), and OS-01 fails when it is out of date. So "generated if cheap"
  was cheap.
- **Proven with a compiler, not asserted.** With the build's own flags and
  `ENGINE_OS_POSIX` forced to 0, `mem.cpp` stops with all 5 of its port items,
  and `frame_arena.h` plus `module_loader.h` with all 4 of theirs. The first
  attempt showed only one error, because a missing include path in my harness
  is a *fatal* error that ends the compile. It was not the code.
- Mutations, each red:
  - `mem.cpp` back to a bare POSIX `#else`
  - a fallback losing its marker
  - a port item reworded without regenerating `porting.md`
  - the rule accepting every `#else`, checked on a synthetic chain so the
    porting-doc check could not mask it. The first version of this one was
    masked exactly that way.
- OS-01 needed **no baseline**: its scope is clean. Outside it there are 22
  bare `#else`s in 11 files, recorded in WO-021.
