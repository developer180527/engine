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
verified: 2026-11-06
---

# platform — the OS window, events, and everything a GUI needs from them

`IPlatform` (one window and the event pump), `IToolWindow` (secondary windows
for torn-off panels), and UI input (`ui_input.h`: pointer, wheel, keys, text,
IME, focus; clipboard, cursor, text-input area; keyboard convention), and
main-window control (WO-070: mode, monitors, size, position, limits, aspect,
flags, show, focus, window events), which is what the C API's `eng_window.h`
stands on. A new OS implements `IPlatform` once; every GUI and the renderer come
along.

## Nothing
`HeadlessPlatform` has no window: `nativeWindowHandle()` is null, which makes the
runtime skip renderer init. Every optional capability defaults to "no" — no UI
input, `createToolWindow` returns null (a GUI keeps floating panels inside the
main window), `globalPointer` returns false, HDR returns false, and
`windowControl()` is false with every window setter returning false and no
monitors listed — so a platform that has not implemented something delivers
nothing rather than failing, and the C API reports it as unavailable by name
(SDL3 is in that state today; GLFW implements it).

## Ownership
The platform owns its window and native handles. A tool window is owned by the
caller's `unique_ptr`; destroying it destroys the OS window. UI event queues
are drained into the caller's vector by `takeUiEvents` (moved, then cleared);
window events the same way by `takeWindowEvents`. Monitors are named by small
ids that keep naming a display while it stays connected.

## Threading
Main thread only. SDL's event queue is process-wide and main-thread-bound on
macOS, so one component pumps (`pollEvents`) and others observe
(`setNativeEventHook`).

## Timing
`pollEvents` once per frame drains every window's events; UI events queue until
the next `takeUiEvents`. Window events are found during `pollEvents` by
comparing the window's state with the previous pump's, so a change a setter
asked for is reported after the next pump, with what the OS actually did. With
`kWindowHandleClose`, a user's close is only a `CloseRequested` event and
`shouldClose()` stays false until the program calls `requestClose()`. Size
limits bind sizes set by code as well as the user's resizing. Positions for tool windows, `contentOrigin` and
`globalPointer` are desktop points.

## Errors
`init` returns false; optional capabilities return false/null. Nothing throws.
