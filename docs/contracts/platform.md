---
status: as-built
contract: platform
kind: interface
state: provisional
owner: src/runtime/platform
header: src/runtime/platform/platform.h
implementations:
  - real: src/runtime/platform/glfw_platform.h
  - real: src/runtime/platform/sdl3_platform.h
  - null: src/runtime/platform/headless_platform.h
covers:
  - src/runtime/platform/platform.h
  - src/runtime/platform/ui_input.h
verified: 2026-09-30
---

# platform — the OS window, events, and everything a GUI needs from them

`IPlatform` (one window and the event pump), `IToolWindow` (secondary windows
for torn-off panels), and UI input (`ui_input.h`: pointer, wheel, keys, text,
IME, focus; clipboard, cursor, text-input area; keyboard convention). A new OS
implements `IPlatform` once; every GUI and the renderer come along.

## Nothing
`HeadlessPlatform` has no window: `nativeWindowHandle()` is null, which makes the
runtime skip renderer init. Every optional capability defaults to "no" — no UI
input, `createToolWindow` returns null (a GUI keeps floating panels inside the
main window), `globalPointer` returns false, HDR returns false — so a platform
that has not implemented something delivers nothing rather than failing.

## Ownership
The platform owns its window and native handles. A tool window is owned by the
caller's `unique_ptr`; destroying it destroys the OS window. UI event queues
are drained into the caller's vector by `takeUiEvents` (moved, then cleared).

## Threading
Main thread only. SDL's event queue is process-wide and main-thread-bound on
macOS, so one component pumps (`pollEvents`) and others observe
(`setNativeEventHook`).

## Timing
`pollEvents` once per frame drains every window's events; UI events queue until
the next `takeUiEvents`. Positions for tool windows, `contentOrigin` and
`globalPointer` are desktop points.

## Errors
`init` returns false; optional capabilities return false/null. Nothing throws.
