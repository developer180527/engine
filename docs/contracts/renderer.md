---
status: as-built
contract: renderer
kind: interface
state: provisional
owner: src/render
header: src/render/renderer_interface.h
implementations:
  - real: src/render/renderer.h
  - null: src/render/renderer_null.h
tests:
  - tests/null_renderer_test.cpp
covers:
  - src/render/renderer_interface.h
verified: 2026-11-27
---

# renderer — what the runtime asks of a renderer

`IRenderer`: scene/game/backbuffer rendering, render targets, display
transforms, diagnostics. `Renderer` is the bgfx implementation; `NullRenderer`
is what a headless host gets.

## Nothing
`NullRenderer` does nothing, correctly: every call is valid, draws nothing and
reports "nothing" (empty diagnostics). It replaced scattered `if (!headless)`
guards, one of which had shipped a ~480 KB/s leak on dedicated servers — callers
never ask whether a GPU exists.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
- `init()` returns false when the renderer cannot start, and `initError()` then
  says why, by name (WO-089): no backend in the project's order created a
  device (each one tried, and why not), or the device lacks a capability the
  renderer's manifest lists under `device.needs`. The runtime reports that text
  as its boot error. A renderer that does not say gets a generic message.
- A capability listed under `device.fallbacks` never fails `init()`: the
  renderer logs that it is using its fallback (`core/device_select.h`).
- The rest is not yet written.
