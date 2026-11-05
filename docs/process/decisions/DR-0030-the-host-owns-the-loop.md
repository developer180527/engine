---
status: decided
id: DR-0030
title: The host owns the loop and the windows; the engine blocks only when asked to pace
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
The program that embeds the engine owns the main loop and the windows.
`engStep` does one frame and returns; it never pumps events on its own and never
waits on a minimised window. It sleeps only when the host asks it to pace
(`ENG_PACING_ENGINE`, WO-061); with `ENG_PACING_HOST`, a display link,
`requestAnimationFrame` or the host's own loop sets the rhythm and `engStep`
never sleeps. `engRun` is a convenience loop built from the same calls.

## Rejected
The engine owning `main()` and the loop (today's `EngineRuntime::run`): it
cannot embed in SwiftUI, Qt or a browser, where the platform owns the loop.

## Why
Embedding (SwiftUI, Qt, a browser canvas), iOS and the web all forbid a
blocking loop. A step function serves every host; a loop serves only one
(`docs/plans/platform-embedder.md`).

## What would reverse it
None expected: a blocking loop remains available as `engRun`.
