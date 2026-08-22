---
status: as-built
tier: prototype
verified: 2026-08-18
covers:
  - src/editor/
tests:
  - tests/editor_undo_test.cpp    # UndoStack: the editor's most consequential logic
  - tests/editor_prefs_test.cpp   # editor.json, untrusted on the project-open path
# The PANELS remain untested — an ImGui application whose behavior is
# mouse-driven is genuinely hard to test, and the engine keeps the editor OUT
# of the runtime (nothing else depends on it), so the blast radius of an
# editor bug is the editor. Still `prototype` for that reason, and honestly so.
# What IS covered now is the part that argument never justified: the two
# headless, ImGui-free pieces where a bug destroys the user's authored work
# rather than mis-drawing a widget. See "Tested surface" below.
---
# Editor

## Window chrome — no title bar
The editor draws to the top edge: the main menu bar occupies the row where the
title bar used to be.

It is **not** an undecorated window, and that is the whole design. Setting
`GLFW_DECORATED=false` / `SDL_WINDOW_BORDERLESS` is the obvious way to get the
look and it throws away move, resize, minimise, maximise, close, snapping,
double-click-to-zoom and the window-manager animations — every one of which
then has to be re-implemented by hand, per platform. That is how editors end up
with a window you cannot resize from the top edge.

Instead the window stays a fully decorated OS window and only the title bar's
DRAWING is suppressed (`runtime/platform/title_bar.h`). On macOS that is
`NSWindowStyleMaskFullSizeContentView` + `titlebarAppearsTransparent` +
`NSWindowTitleHidden`. Traffic lights keep working, the top edge still resizes,
the window still zooms on a double click.

The cost, stated plainly: the window buttons now float over the editor's own
content, so the menu bar insets its first item past them and matches the band's
height. Both numbers are **measured** from the live window every frame, never
hardcoded — the traffic lights move with the system's appearance and
accessibility settings, their spacing has changed across macOS releases, and
the band height differs by OS version.

Hiding the bar also takes away what it did for free. The content view swallows
the events the window manager used to get, so **drag** and
**double-click-to-zoom** stop working. Both are handed back explicitly: the
empty strip to the right of the menus is a drag region that calls
`platwin::beginWindowDrag()`, which asks the OS to run its own move loop.
Deliberately not "track the mouse and set the window origin each frame" — that
reimplementation loses snapping, drag-to-another-display and the release
animation, and fights the compositor for every pixel. Double-click reads the
system's own `AppleActionOnDoubleClick` preference rather than assuming zoom.

The band is shared (`editor/window_chrome.h`), not menu-bar-specific: "the
window has no title bar" is a property of the WINDOW, not of whichever page is
up. The project hub is a different page and needs the identical band. It
originally had none, so its header sat jammed under the traffic lights and the
hub window could not be moved at all.

**Games keep their title bar.** `hideTitleBar` defaults to false and only the
editor sets it; `engine_host` and `engine_player` come up with normal system
chrome. A shipped game window is a normal window.

### Windows and Linux
**Windows is written but UNVERIFIED** (`title_bar_windows.cpp`) — authored
without a Windows toolchain to compile it against, and no CI builds a Windows
target yet. Treat it as a reviewed proposal. It is a Windows-only TU, so it
cannot affect the macOS or Linux builds either way.

Windows is genuinely not the same job, and the difference is not cosmetic: its
minimise/maximise/close buttons are part of the NON-CLIENT area that
`WM_NCCALCSIZE` removes, so hiding the bar DELETES them. macOS's traffic lights
survive because they belong to the window's own chrome. Hence
`titleBarNeedsCustomButtons()` — true on Windows, false on macOS. A UI written
to the macOS shape would ship on Windows with no way to close the window.

The editor therefore draws its own minimise / maximise / close on the trailing
edge whenever that flag is set. They are ImDrawList PRIMITIVES, not font
glyphs: the Windows convention is Segoe MDL2 Assets, which is Windows-only and
absent from the machines this is developed on. Sized 46x32 logical with a 10px
glyph — the Windows metric, because it is what every other window on the user's
desktop already agrees on — and close goes red on hover while the others go
grey, which is the affordance that stops someone closing the editor when they
meant to maximise it.

The DRAWING is verified: the macOS build was temporarily forced into the
Windows shape to screenshot it, confirming placement, sizing and spacing. What
remains unverified is only the Win32 side that the buttons call into.

Linux has no implementation and falls back to a real title bar. Whether one
exists at all is the COMPOSITOR's call (server- vs client-side decorations) and
it differs across GNOME, KDE and the wlroots compositors — there is no portable
call to make.

## Tested surface
Two lanes, both headless — no window, no ImGui context, no GPU.

**`editor_undo_test`** — `UndoStack`. The most consequential logic in the
editor: a wrong undo silently destroys authored work, and the user's only
signal is a scene that is subtly wrong later. 34 assertions over index
bookkeeping, redo-tail truncation, `kMaxDepth` eviction (the front-eviction /
back-index arithmetic is right until it is off by one), delete/undo preserving
the ORIGINAL EntityId — the guarantee that keeps earlier commands in the stack
resolving — parent links surviving a delete, reparent refusing to build a
cycle, component toggle round-trips, and commands whose target has since been
destroyed degrading to a logged no-op. It passed all of them on the first run;
the value is that it now cannot regress silently.

One assertion is deliberately indirect: a delete/undo of an entity with a
`Camera` must restore the camera, even though `UndoStack` never names that
component. That is proof the snapshot really does go through the shared
`EntitySerde` table rather than a private copy — the design guarantee the
header claims.

**`editor_prefs_test`** — `editor.json`. The one piece of editor state that
comes back FROM DISK, which puts it in the same category as the scene
deserializers. It found a live crash: `{"camera":{"position":[]}}` **segfaulted**
`EditorPrefs::load`. The `try { ... } catch (...) {}` around it reads as total
safety and is not — nlohmann's const `operator[](size_type)` is undefined
behaviour out of range, not an exception, so nothing was there to catch. This
is on the project-OPEN path, so a corrupt `editor.json` made the project
impossible to open.

That was the fourth instance of one defect (five sites in
`scene/entity_serializer.h`, `UndoStack::desTf`, and this), so the fix is a
shared `core/json_read.h` rather than a fourth copy of the same three lines.

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
- **Window seam** — `wsi::`, from **`runtime/platform/window_ops.h`**. The
  editor drives several OS windows (a detached Scene/Game View is its own ImGui
  viewport), which `IPlatform` cannot model since it owns a single window, and
  the handles come from `ImGuiViewport::PlatformHandle`. So per-window
  focus/key-poll/cursor-capture goes through an opaque handle, with one
  implementation TU per backend compiled into `engine_runtime`.

  **This used to live here**, as `src/editor/window_ops.h` in namespace
  `edwin`, and that made multi-window support an ImGui-editor privilege: any
  other host — Qt, a Rust tool, a custom launcher — got the single-window
  `IPlatform` and had to reinvent it against GLFW directly. The editor is a
  *consumer* of the SDK, not a layer of it, so the seam moved to the runtime
  beside its matching `IPlatform`.

  What is still backend-specific to the editor is exactly one file: the ImGui
  platform backend. Swapping to SDL3 = that backend + the platform choice in
  `main.cpp`; no panel changes, and the window seam comes along for free.

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

## Two consoles, because there are two audiences

`panels/console_panel.h` is the **Console** a game developer sees: their content,
their scripts, and any warning or error from anywhere. `panels/internal_console_panel.h`
is the **Internal Console** — subsystem targeting (Solo streams one subsystem at
every level and drops the rest to warnings/errors), per-category volume, and the
log ring's own health (occupancy, evicted, truncated). It is **off by default**
under View > Panels; a game developer should never have to know it exists.

They read the same ring; only the filter and the instrumentation differ.
`elog::visibleToGame` is the rule, and the asymmetry matters more than the split:
info-level chatter is an allowlist, but **warnings and errors from every category
always reach the game console**, so a subsystem nobody marked can never hide a
failure from the person whose build is broken.

This was a correction. The first version of the logger rewrite put the subsystem
grid inside the game console, which hands a game developer a diagnostic instrument
for somebody else's problem and buries theirs underneath it.
