# Editor

## Purpose
The ImGui editor application on top of the `engine_runtime` SDK. This is the
only layer that links ImGui/ImGuizmo — the runtime stays UI-free.

## Architecture
- **`main.cpp`** — entry point: picks the project root, creates the
  `GlfwPlatform`, inits the runtime, adds the editor + `CookService`.
- **`EditorApp`** (`editor_app.h`) — wires panels into the runtime's frame
  loop via `m_rt.run([this](float dt){ frame(dt); })`. Owns editor-only state:
  camera, selection, undo stack, sim state, the play-mode game world.
- **Panels** — each a `draw*Panel(ctx)` free function or small header:
  hierarchy, inspector, asset browser, console/terminal, game view, scene
  view, project settings, menu bar. All receive `EngineContext` per frame.
- **`EngineContext`** (`src/engine_context.h`) — `RuntimeContext` +
  `EditorState` + `GizmoState`, built on the stack each frame in `buildCtx()`.
  Never stored.
- **ImGui glue** — `imgui_bgfx.*` (renderer backend, embedded shaders +
  fonts), `imgui_impl_glfw.*` (platform backend), docking + multi-viewport.
- **`IEditorPlugin`** (`editor_plugin.h`) — optional UI extension for engine
  plugins; the editor dynamic_casts entries of the runtime's PluginRegistry.

## Play Mode
Play calls `m_rt.startSimulation(SimMode::Snapshot)` — the runtime snapshots
the editor world and simulates a fresh copy (`m_rt.simWorld()`). Per frame
while Playing (not Paused) the editor calls `m_rt.tickSimulation(dt)`, which
runs the plugin phases in order (script intent lands in the same physics
step — no input-latency frame). Stop calls `m_rt.stopSimulation()`; the
editor world is untouched. Pause is purely "don't call tickSimulation".

## Invariants
- `EngineContext` is stack-only, one frame lifetime.
- `imguiNewFrame()` before any ImGui call; gizmo BeginFrame outside any
  Begin/End block.
- Editor camera reads input only when the Scene View is hovered (or during
  right-drag); ImGui keyboard/mouse capture gates gameplay input.
- Scene saves also cook the binary scene (keeps both formats in sync).

## Future Work
- Plugins menu surfacing IEditorPlugin::onEditorUI (currently not drawn).
