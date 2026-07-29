# Editor

## Purpose
The ImGui editor application on top of the `engine_runtime` SDK. This is the
only layer that links ImGui/ImGuizmo — the runtime stays UI-free.

## Architecture
- **`main.cpp`** — entry point. `editor <dir>` opens that project directly;
  `editor` with no args boots projectless and shows the **project hub**
  first. main owns imguiInit/theme (both hub and editor draw ImGui).
- **`ProjectHub`** (`project_hub.h`) — fullscreen Projects page shown before
  the editor (same executable, separate boot phase — deliberately NOT a
  second app: no IPC, instant transition). Lists `~/.engine/projects.json`
  (engine_core `KnownProjects`), creates projects via `project::create`
  (same code as the engine_project CLI), opens by path. Returns the picked
  root; main calls `runtime.openProject` and proceeds into the editor.
- **`EditorApp`** (`editor_app.h`) — wires panels into the runtime's frame
  loop via `m_rt.run([this](float dt){ frame(dt); })`. Owns editor-only state:
  camera, selection, undo stack, sim state, the play-mode game world.
- **Panels** — each a `draw*Panel(ctx)` free function or small header:
  hierarchy, inspector, asset browser, console/terminal, profiler, game view, scene
  view, project settings, menu bar. All receive `EngineContext` per frame.
- **`EngineContext`** (`src/engine_context.h`) — `RuntimeContext` +
  `EditorState` + `GizmoState`, built on the stack each frame in `buildCtx()`.
  Never stored.
- **ImGui glue** — `imgui_bgfx.*` (renderer backend, embedded shaders +
  fonts), `imgui_impl_glfw.*` (platform backend), docking + multi-viewport.
  `imguiInit` also registers the **editor UI backend** (`engineUiSetBackend`)
  so any plugin/kit can draw via the `engineUi*` facade without linking ImGui.
- **Window seam** (`window_ops.h` + `window_ops_glfw.cpp`) — the editor's ONLY
  window-system dependency. The editor drives several OS windows (a detached
  Scene/Game View is its own ImGui viewport), which `IPlatform` cannot model
  since it owns a single window, and the handles come from
  `ImGuiViewport::PlatformHandle`. So per-window focus/key-poll/cursor-capture
  goes through `edwin::` on an opaque handle, with one implementation TU per
  backend. Swapping to SDL3 = `window_ops_sdl3.cpp` + the ImGui platform
  backend + the platform choice in `main.cpp`; no panel changes.
  This is TOOLING input — engine systems and kits bind to actions through
  InputManager and must never poll windows.
- **Plug-in Manager** (`panels/plugins_panel.h`) — lists running plugins +
  manifest kits with their *true* load status (`KitHost::status()`), raises a
  modal on kit-load failure, and calls each plugin's `onEditorUI()` (drawn
  through the facade above). `IEditorPlugin` is retired — editor UI is now a
  runtime concept (`IEnginePlugin::onEditorUI`) routed through the facade, so
  it works for dynamically-loaded kits too.
- **Add Component** (`panels/inspector_panel/add_component.h`) — one searchable
  menu over a small addable-component registry (the manual stand-in for the
  coming reflection system).

## Play Mode
Play calls `m_rt.startSimulation(SimMode::Snapshot)` — the runtime snapshots
the editor world and simulates a fresh copy (`m_rt.simWorld()`). Per frame
while Playing (not Paused) the editor calls `m_rt.tickSimulation(dt)`, which
runs the plugin phases in order (script intent lands in the same physics
step — no input-latency frame). Stop calls `m_rt.stopSimulation()`; the
editor world is untouched. Pause is purely "don't call tickSimulation".

### The Play boundary (keep editor stuff out of play)
Two kinds of isolation, by construction:
- **World** — play runs on a *snapshot copy* (`simWorld()`), edited entirely
  separately from the editor world. Editor-world edits persist across Play;
  play-world changes are discarded on Stop. Nothing to police here.
- **Input / interaction** — this is the part that leaks if you're not careful.
  Rule: while playing, the *game* owns input and editor-authoring tools are
  inert. The predicate is `EditorState::playing()`. Enforced today:
  the mouse gate is skipped while the game locks the cursor (so ImGui's
  `WantCaptureMouse` can't eat clicks); `gizmoHandleHotkeys` and `drawGizmo`
  early-out. **Any new editor-authoring feature must gate on `playing()`** —
  that's the one check that keeps the boundary clean. (The Scene-View free
  camera is deliberately *not* gated: flying it during play is a debug view.)

## Invariants
- `EngineContext` is stack-only, one frame lifetime.
- `imguiNewFrame()` before any ImGui call; gizmo BeginFrame outside any
  Begin/End block.
- Editor camera reads input only when the Scene View is hovered (or during
  right-drag); ImGui keyboard/mouse capture gates gameplay input.
- Scene saves also cook the binary scene (keeps both formats in sync).

## Future Work
- Debug-draw API (`engineDraw*`) so kits can render gizmos/overlays in the
  scene view (damage radii, motion-matching trajectories) — the 3D counterpart
  to the `engineUi*` panel facade.
- Reflection so kit-defined components serialize + show in the Inspector
  (replaces the hand-rolled add-component registry).
