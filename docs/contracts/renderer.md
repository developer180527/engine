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
verified: 2026-09-27
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
Not yet written.
