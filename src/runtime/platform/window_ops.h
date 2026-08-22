#pragma once
#include "runtime/input/input_event.h"     // Key, MouseButton (backend-free)
#include "runtime/platform/platform.h"     // CursorMode

// ── wsi — per-window operations, for hosts that drive more than one window ───
//
// `IPlatform` models exactly ONE window: it has `nativeWindowHandle()`, not
// `nativeWindowHandle(w)`. That is right for a game, which has one window and
// wants nothing to do with the concept of a second. It is wrong for a TOOL: a
// detached Scene View is its own OS window, and a host must query focus, poll
// keys and lock the cursor PER WINDOW.
//
// This header is that second seam. Everything is a free function taking an
// opaque handle, and exactly one implementation TU is compiled in:
//
//   window_ops_glfw.cpp   GLFWwindow* handles
//   window_ops_sdl3.cpp   SDL_Window*  handles
//
// selected by ENGINE_WINDOW_BACKEND, alongside the IPlatform implementation it
// belongs beside.
//
// ── Why this lives in the runtime and not in the editor ─────────────────────
// It used to be `src/editor/window_ops.h`, namespace `edwin`, and that was a
// mistake with a consequence: it made multi-window support an ImGui-editor
// privilege. Anyone building a different editor — Qt, a Rust tool, a custom
// launcher — got the single-window IPlatform and had to reinvent this from
// scratch, against GLFW or SDL directly, which is precisely the coupling the
// SDK exists to prevent.
//
// The editor is a CONSUMER of the engine SDK, not a layer of it. Anything the
// editor needs in order to drive windows, every other host needs too.
//
// NOTE ON GLFW LINKAGE, so this is not mistaken for a fix it is not: moving
// this file does NOT make GLFW conditional. engine_runtime still links GLFW in
// both backends because runtime/input/input_system.h is GLFW-callback based and
// is pulled in by runtime.cpp. That is the input stack's problem, tracked
// separately; this change makes the WINDOW seam reusable, nothing more.
//
// ── SCOPE — this is TOOLING input, not gameplay input ───────────────────────
// A host application may poll its own windows. Engine systems and kits must
// never use this: they bind to actions through InputManager and stay
// device-blind (see the input architecture directives). Anything here that
// starts being useful to gameplay is a sign it belongs in the input stack
// instead.
namespace wsi {

// An opaque windowing-library window. In practice this is
// ImGuiViewport::PlatformHandle for an ImGui host, or the platform's own
// backendWindowHandle() for the main window. Never dereferenced outside the
// backend TU.
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
// own table for those. Used by key-binding UI.
const char* keyName(Key k);

// ── Bulk polling, for the window input source ───────────────────────────────
// InputSystem sweeps the entire key space every frame to double-buffer it.
// Doing that through isKeyDown() would be ~350 out-of-line calls per frame for
// data the backend can hand over in one sweep — and, worse, would make the
// per-frame cost of the input system a property of how the seam is compiled.
//
// `out` is indexed by the ENGINE's code space (Key / MouseButton values), not
// the backend's, and is fully overwritten: entries with no backend equivalent
// are set false rather than left alone.
void pollKeyboard(WindowHandle w, bool* out, int count);
void pollMouseButtons(WindowHandle w, bool* out, int count);

// ── Scroll and text ─────────────────────────────────────────────────────────
// The two inputs NEITHER backend can poll. GLFW delivers them as callbacks and
// SDL as events, and that difference is the single reason input_system.h used
// to carry `#if defined(ENGINE_WINDOW_BACKEND_SDL3)` and a raw <GLFW/glfw3.h>
// include — which is what linked GLFW into every build of engine_runtime,
// including SDL3 ones. Both shapes reduce to "call me when this happens", so
// the seam takes a sink and the backends reconcile the rest.
//
// Text is delivered as CODEPOINTS. GLFW's char callback already provides them;
// the SDL backend decodes its UTF-8 before calling, so no consumer ever sees
// an encoding.
struct InputSink {
    void* ctx;
    void (*onScroll)(void* ctx, double dx, double dy);
    void (*onText)(void* ctx, uint32_t codepoint);
};

// Installs the sink for `w`. On GLFW this chains to any previously-registered
// scroll/char callbacks (ImGui's), which is why it must be called AFTER the UI
// backend initialises. On SDL there is nothing to chain — events arrive through
// feedNativeEvent instead.
void installInputSink(WindowHandle w, InputSink sink);

// Feeds one native event (an SDL_Event* on SDL3) to the installed sink.
// A no-op on GLFW, which has no event struct to forward. The host wires this to
// IPlatform::setNativeEventHook.
void feedNativeEvent(const void* nativeEvent);

} // namespace wsi
