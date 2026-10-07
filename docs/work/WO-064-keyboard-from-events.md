---
status: plan
id: WO-064
title: The keyboard snapshot is built from key events, not about 315 key polls a frame
program: providers
priority: P2
size: S
state: todo
depends: []
contracts: [input-source]
touches:
  - src/runtime/platform/window_ops_glfw.cpp
  - src/runtime/platform/window_ops_sdl3.cpp
  - src/runtime/platform/window_ops.h
source: review 2026-10-07, item 7
---
## Why
`pollKeyboard` calls `glfwGetKey` for every key, about 315 times a frame, to build the keyboard snapshot: about 10 % of the main thread with no camera rendering.

GLFW and SDL3 already deliver key events. Building the snapshot from them costs nothing on a frame with no key change. It also fixes a correctness gap polling has: a key pressed and released between two polls is never seen.

## Done when
- [ ] the GLFW and SDL3 backends keep the key state from their key callbacks/events; nothing iterates over every key per frame
- [ ] a press and release inside one frame reaches the snapshot as a press (a test feeds two events between frames and sees it)
- [ ] the snapshot is identical to the polled one on ordinary input, checked by the existing input tests
- [ ] focus loss clears held keys (a key released while the window is unfocused is not stuck down)
- [ ] main-thread time for input measured before and after

## Contract
Nothing: `pollKeyboard`'s output is unchanged for callers. The headless platform still reports no keys down.
