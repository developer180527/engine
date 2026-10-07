---
status: plan
id: WO-061
title: The engine paces its own frames, and draws none without a reason
program: providers
priority: P1
size: M
state: todo
depends: []
touches:
  - src/runtime/runtime_frame.cpp
  - src/render/renderer/device.cpp
  - src/runtime/platform/platform.h
  - src/runtime/platform/glfw_platform.cpp
  - src/runtime/platform/sdl3_platform.cpp
  - src/editor/editor_app.h
  - src/tools/engine_player.cpp
  - tests/empty_game_budget_test.cpp
source: review 2026-10-07 (empty project, build-prof, Metal), item 1; owner rule on idle cost (DR-0011)
---
## Why
With no camera, an empty project runs at about 19 000 frames a second on 1.6 CPU cores, because the loop's only brake is waiting for vsync, and nothing waits for vsync when nothing is presented.

That happens in every real game too: loading screens, scene transitions, a UI-only menu. And waiting for vsync is not a guarantee even when something is presented. A covered macOS window, vsync switched off, a driver that ignores it, or a variable-refresh display can each make it return at once. The engine's own rule is that it does no work without a reason (DR-0011's idle-cost rule), so the engine must decide when the next frame starts and sleep until then, whatever the GPU API does.

Measured 2026-10-07, before:

| | empty project | camera + light + cube |
|---|---|---|
| frame rate | ~19 000 fps | 120 fps (vsync) |
| CPU | 162 % | 11.6 % |

The editor also draws continuously: 3 164 frames in 26.4 s (120 fps) on the WO-033 check project with nobody touching it.

## Done when
- [ ] **a decision record**: the engine owns frame pacing; waiting on vsync can tighten the timing but never replaces the pacer. It names the target rate (the display's refresh, or a cap the game sets) and what happens when the window cannot be seen
- [ ] **the pacer**: when a frame finishes before its deadline, the main thread blocks on an OS timer until the deadline. It never spins and never polls. The target is the display's refresh rate, or the game's cap if lower (a project setting, settable at run time, so a game can drop to 30 on battery)
- [ ] **nothing drawn is still a frame**: when no view rendered, the back buffer is cleared and presented, so vsync and the pacer agree
- [ ] **unseen windows**: minimised, or fully covered where the OS reports it (macOS occlusion state; on Windows and Linux, what the platform offers, or written down as unavailable). While unseen, nothing is rendered. The simulation keeps its fixed rate unless the game pauses it. The minimised wait blocks on events with no timeout (today it is `waitEvents(0.1)`: ten wake-ups a second for nothing)
- [ ] **the editor redraws only for a reason**: input, a change to the scene or a panel, a running simulation, an animation playing, an asset finishing loading. An editor left alone draws about nothing, measured
- [ ] **measured after**, on the same machine and build as before: empty project at or under the display's refresh rate with the CPU budget recorded in this order's log; camera + light + cube unchanged or better; the idle editor's frames per second
- [ ] **a gate**: a windowed check (runs where there is a display; skipped, and says so, where there is not) opens an empty project for a few seconds and fails if frames exceed the refresh rate by more than 5 % or the process CPU exceeds the recorded budget. Removing the pacer turns it red, checked
- [ ] **the same check with vsync off**: the pacer alone holds the rate

## Contract
Nothing: a game that sets no cap gets the display's refresh rate. A platform with no way to know the refresh rate uses 60 Hz and logs it once. The headless runtime has no frames to pace: its fixed tick is unchanged.

## Not in scope
- bgfx's memory and per-frame fixed cost (WO-062)
- keyboard polling (WO-064)
- the job pool's idle behaviour (WO-065)
