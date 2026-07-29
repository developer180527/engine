#pragma once
#include "runtime/input/input_event.h"     // Key, MouseButton (backend-free)
#include "runtime/platform/platform.h"     // CursorMode

// ── window_ops — the editor's ONLY window-system dependency ──────────────────
// The editor drives more than one OS window: a detached Scene View or Game View
// is its own ImGui platform viewport, and the editor has to query focus, poll
// keys, and lock the cursor PER WINDOW. `IPlatform` deliberately models a
// single window, so it cannot own those operations — and the handles in
// question come from `ImGuiViewport::PlatformHandle`, i.e. from whichever
// ImGui platform backend is compiled in.
//
// So the editor talks to windows exclusively through the free functions below,
// and exactly one implementation TU provides them:
//
//   window_ops_glfw.cpp   GLFWwindow* handles      (today)
//   window_ops_sdl3.cpp   SDL_Window* handles      (when SDL3 lands)
//
// The payoff: swapping the windowing library touches this one file plus the
// vendored ImGui platform backend, instead of a dozen editor headers. Nothing
// under src/editor/ outside this seam names GLFW.
//
// SCOPE — this is TOOLING input, not gameplay input. The editor is an
// application and may poll its own windows. Engine systems and kits must never
// use this: they bind to actions through InputManager and stay device-blind
// (see docs + the input architecture directives). Anything here that starts
// being useful to gameplay is a sign it belongs in the input stack instead.
namespace edwin {

// An opaque windowing-library window. In practice this is
// ImGuiViewport::PlatformHandle, or GlfwPlatform::glfwWindow() for the main
// window. Never dereferenced outside the backend TU.
using WindowHandle = void*;

// ── Per-window state ────────────────────────────────────────────────────────
// All of these tolerate a null handle (returning the quiet default) so callers
// don't need a guard at every site: a detached panel's handle is null for the
// frame between docking states changing.
bool       isFocused(WindowHandle w);
bool       isKeyDown(WindowHandle w, Key k);
bool       isMouseButtonDown(WindowHandle w, MouseButton b);
void       cursorPos(WindowHandle w, double& x, double& y);
CursorMode cursorMode(WindowHandle w);
void       setCursorMode(WindowHandle w, CursorMode mode);
void       framebufferSize(WindowHandle w, int& fbW, int& fbH);
void       requestClose(WindowHandle w);

// The OS-level handle bgfx renders into (NSWindow* / HWND / X11 Window /
// wl_surface) for THIS window — needed per viewport to create a swap chain.
void*      nativeWindowHandle(WindowHandle w);

// ── Keyboard labelling ──────────────────────────────────────────────────────
// Layout-aware printable name for a key ("q", "ü", …), or null when the key
// has no printable form (F1, Escape, modifiers) — callers fall back to their
// own table for those. Used by the key-binding UI.
const char* keyName(Key k);

} // namespace edwin
