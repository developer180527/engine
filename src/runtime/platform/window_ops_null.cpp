// ── window_ops_null — the per-window seam on a machine with no windows ──────
//
// The SERVER BUILD's implementation of wsi:: (G1c step B). window_ops.h says
// exactly one implementation TU is compiled in — glfw, sdl3, or this one — and
// this is the third case: no windowing library linked at all, which on Linux is
// the difference between a binary that starts on a headless box and one that
// fails to load libX11.
//
// ── Why this is nearly free to write ────────────────────────────────────────
// window_ops.h already specifies every answer here. Its contract for the handles
// it hands out is:
//
//   "All of these tolerate a null handle (returning the quiet default) so
//    callers don't need a guard at every site."
//
// A server has no windows, so EVERY handle is null and every call is already
// specified to return the quiet default. This file is that sentence, compiled.
// It is not a new behaviour and callers cannot tell it apart from a real backend
// asked about a window that does not exist.
//
// ── Who actually calls this in a server ─────────────────────────────────────
// InputSystem::windowFocused(), which is why the server link failed without it.
// Input from a window is meaningless on a dedicated server; what matters is that
// the input STACK still compiles and runs, because gameplay code reads
// InputManager actions and must not need a server-specific branch. It simply
// reads "nothing pressed, not focused", forever.
//
// That is also the honest limit of this file: it makes a server LINK, it does
// not make network-driven input work. A server's real inputs arrive from the
// network layer, and routing those into InputManager is a different problem
// (modules/net is prototype).
#include "runtime/platform/window_ops.h"

namespace wsi {

// Not focused: a server has no window to focus, and "false" is what a real
// backend returns for a handle that names no window.
bool isFocused(WindowHandle)                     { return false; }
bool isKeyDown(WindowHandle, Key)                { return false; }
bool isMouseButtonDown(WindowHandle, MouseButton){ return false; }

void cursorPos(WindowHandle, double& x, double& y) { x = 0.0; y = 0.0; }
CursorMode cursorMode(WindowHandle)              { return CursorMode::Normal; }
void setCursorMode(WindowHandle, CursorMode)     {}

// 0x0, meaning "there is no framebuffer" — the same answer NullRenderer gives
// for sceneW/H(), and for the same reason: a plausible fake size invites a
// caller to compute an aspect ratio from a surface that does not exist.
void framebufferSize(WindowHandle, int& w, int& h) { w = 0; h = 0; }

// A server exits on a signal or an admin command, not on a window close. This
// being a no-op is why nothing tries to close a window that was never opened.
void requestClose(WindowHandle) {}

void* nativeWindowHandle(WindowHandle) { return nullptr; }

// No layout, so no layout-aware name. window_ops.h specifies null as "this key
// has no printable form", and callers already fall back to their own table —
// which is the correct outcome for every key here.
const char* keyName(Key) { return nullptr; }

// Fully overwritten, as the contract requires: entries with no backend
// equivalent are set false rather than left alone, and here that is all of them.
// Leaving the buffer untouched would let a caller read whatever the last frame
// had, which on a server would be stale forever.
void pollKeyboard(WindowHandle, bool* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = false;
}
void pollMouseButtons(WindowHandle, bool* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = false;
}

// Nothing to chain and nothing to deliver: scroll and text are the two inputs
// neither real backend can poll, and both arrive from a window system this build
// does not have.
void installInputSink(WindowHandle, InputSink) {}
void feedNativeEvent(const void*) {}

} // namespace wsi
