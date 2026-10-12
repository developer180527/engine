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
source: review 2026-10-07 (empty project, build-prof, Metal), item 1; owner rule on idle cost (DR-0011); industry research 2026-10-07 (below)
---
## Why
With no camera, an empty project runs at about 19 000 frames a second on 1.6 CPU cores: the loop's only brake is waiting for vsync, and nothing waits for vsync when nothing is presented.

That happens in real games too: loading screens, scene transitions, a UI-only menu. And waiting for vsync does not always block even when something is presented: a covered macOS window, vsync switched off, or a driver that ignores it. The engine's rule is that it does no work without a reason (DR-0011), so it needs a limiter that holds whatever the GPU API does.

Measured 2026-10-07, before:

| | empty project | camera + light + cube |
|---|---|---|
| frame rate | ~19 000 fps | 120 fps (vsync) |
| CPU | 162 % | 11.6 % |

The editor also draws continuously: 3 164 frames in 26.4 s (120 fps) on the WO-033 check project with nobody touching it.

## What engines do (researched 2026-10-07, sources at the end)
- **The display sets the timing when it can; a sleep limiter is the cap and the fallback.** Unity ignores its software `targetFrameRate` on desktop when vsync is on, and calls software timing "subject to microstuttering". Godot keeps vsync on by default, and its `max_fps` cannot exceed the refresh rate with vsync on.
- **The limiter keeps a running target time, not a fixed sleep.** Godot's `OS::add_frame_delay` advances a target by one frame, sleeps only the remainder, then clamps the target to within one frame of now. That prevents a burst of catch-up frames after a stall and drift building up. Its own comment warns that a fixed sleep "is not recommended to use … as a FPS limiter".
- **A cap below the refresh rate must divide it.** Android's Swappy shows that 30 fps on a 60 Hz screen paced by sleeping gives 49/16/33 ms frames. Unity on mobile rounds a target down to a divisor of the refresh rate.
- **Unfocused is not the same as unseen.** Bevy's game preset runs at full rate when focused and drops to about 60 Hz unfocused. Godot's editor drops to about 10 fps unfocused, and Unreal's editor has "Use Less CPU when in Background". When the window cannot be seen, Godot sleeps instead of drawing, and Microsoft's DXGI sample stops rendering and blocks on window events.
- **Editors redraw on change.** Godot's editor redraws only when something changed. It has an "update continuously" switch for debugging and an update spinner that turns on every redraw, so a panel redrawing for nothing shows at once. Bevy's app preset redraws only on events or an explicit request.
- **Waiting for the display before a frame starts** is better still (DXGI waitable swap chain, `CAMetalDisplayLink`, Vulkan/Android present timing), but bgfx hides the swap chain. That belongs to the RHI: [`open-decisions.md`](../rhi/open-decisions.md) decision 8.

## Done when
- [ ] **a decision record**: the display (vsync) sets the frame timing whenever it does; the engine's sleep limiter caps the rate below it and holds the rate when waiting for vsync does not block. It names the three window states below, and says the RHI's display clock (decision 8) replaces the sleep where it exists
- [ ] **nothing drawn is still a frame**: when no view rendered, the back buffer is cleared and presented, so vsync is waited for exactly as in a frame that drew
- [ ] **the limiter**: a running target time advanced by one frame period, a sleep on an OS timer for the remainder (never a spin, never a poll), and the target clamped to within one frame of now after each frame (Godot's rule). The period is the display's refresh, or the game's cap if lower
- [ ] **caps divide the refresh**: with vsync on, a cap is rounded down to the refresh rate divided by a whole number (on 120 Hz: 120, 60, 40, 30…), and the rate actually used is reported. With vsync off any cap is allowed. The cap is a project setting and can be changed at run time, so a game can drop to 30 on battery
- [ ] **three window states**:
  - *focused*: full rate (or the cap);
  - *unfocused*: a separate rate, set separately for games and for the editor (defaults written in the decision record, starting from the researched ones: games keep their rate, the editor drops to about 10 fps);
  - *unseen*, meaning minimised, or fully covered where the OS reports it (macOS `NSWindow.occlusionState`; on Windows and Linux, what the platform offers or written down as unavailable): nothing is rendered.
- [ ] **unseen does not poll**: the wait blocks on window events until the next simulation step is due, and with no timeout when the simulation is paused or stopped. Today it is `waitEvents(0.1)`, ten wake-ups a second whatever the simulation needs. The simulation keeps its fixed rate unless the game pauses it
- [ ] **the editor redraws only for a reason**: input, a change to the scene or a panel, a running simulation, a playing animation, an asset finishing loading, or an explicit request. An "update continuously" switch exists for debugging, and a redraw indicator (Godot's spinner) shows each redraw in development builds. An editor left alone draws about nothing, measured
- [ ] **measured after**, same machine and build as before: the empty project at or under the refresh rate, with its CPU recorded here as the budget; camera + light + cube unchanged or better; the idle editor's frame rate, focused and unfocused
- [ ] **a gate**: a windowed check (runs where there is a display; skipped, and says so, where there is not) opens an empty project for a few seconds and fails if frames exceed the refresh rate by more than 5 % or CPU exceeds the recorded budget. Removing the limiter turns it red, checked
- [ ] **the same check with vsync off, and with a 30 fps cap**: the limiter alone holds the rate, and frame-time spread at the cap is recorded (the stutter Swappy describes is what this would show)

## Contract
Nothing: a game that sets no cap gets the display's refresh rate. A platform that cannot report the refresh rate uses 60 Hz and logs it once. A platform that cannot report occlusion treats a covered window as unfocused. The headless runtime has no frames to pace: its fixed tick is unchanged.

## Not in scope
- waiting for the display before the frame starts (RHI, decision 8)
- bgfx's memory and per-frame fixed cost (WO-062)
- keyboard polling (WO-064)
- the job pool's idle behaviour (WO-065)

## Sources (read 2026-10-07)
- Godot: [`Engine.max_fps`](https://docs.godotengine.org/en/stable/classes/class_engine.html), [`OS` low-processor mode](https://docs.godotengine.org/en/stable/classes/class_os.html), [`add_frame_delay`](https://github.com/godotengine/godot/blob/master/core/os/os.cpp), [`EditorSettings`](https://github.com/godotengine/godot/blob/master/doc/classes/EditorSettings.xml)
- Unity: [`targetFrameRate`](https://docs.unity3d.com/ScriptReference/Application-targetFrameRate.html), [`OnDemandRendering`](https://docs.unity3d.com/ScriptReference/Rendering.OnDemandRendering.html)
- Unreal: [`UEditorPerformanceSettings`](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEditorPerformanceSettings), [editor frame-rate caps](https://larstofus.com/2024/05/19/unreals-secret-editor-framerate-caps/)
- Bevy: [`UpdateMode`](https://docs.rs/bevy/latest/bevy/winit/enum.UpdateMode.html), [`WinitSettings`](https://github.com/bevyengine/bevy/blob/main/crates/bevy_winit/src/winit_config.rs)
- Platforms: [Android Frame Pacing](https://developer.android.com/games/sdk/frame-pacing), [DXGI 1.3 waitable swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains), [`CAMetalDisplayLink`](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink), [`NSWindow.occlusionState`](https://developer.apple.com/documentation/appkit/nswindow/occlusionstate-swift.property)
