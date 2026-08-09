#pragma once
// ── title_bar — hide the OS title bar without giving up the OS window ────────
//
// The editor wants its content edge-to-edge with the menu bar where the title
// bar used to be. The obvious way to get that is an UNDECORATED window
// (`GLFW_DECORATED=false` / `SDL_WINDOW_BORDERLESS`), and it is the wrong way:
// undecorating throws away move, resize, minimise, maximise, close, snapping,
// double-click-to-zoom, and the window-manager animations, and every one of
// those then has to be re-implemented by hand, per platform, badly. That is
// how editors end up with a window you cannot resize from the top edge.
//
// So this does the opposite: the window stays a normal, fully decorated OS
// window, and only the title bar's DRAWING is suppressed while its behaviour
// is kept. On macOS that is exactly what the Cocoa flags below express — the
// content view is extended under the title bar, the bar itself is made
// transparent and its text hidden. Traffic lights keep working, the top edge
// still resizes, and the window still zooms on a double click.
//
// COST, stated plainly: the traffic lights now float OVER the editor's own
// content. Whatever draws at the top has to leave room for them — see
// `titleBarInset()`, which the menu bar applies as leading padding.
//
// Per platform:
//   macOS    implemented (title_bar_macos.mm, Cocoa)
//   Windows  NOT YET — the equivalent is a WM_NCCALCSIZE handler that zeroes
//            the non-client top edge while leaving the resize frame intact.
//            Deliberately not faked with a borderless window; see above.
//   Linux    NOT YET — depends on the compositor and on whether the WM does
//            server-side decorations at all.
// The unimplemented platforms return false and leave the window alone, so the
// editor gets a normal title bar rather than a broken one.
#include <cstdint>

namespace platwin {

// `nativeWindow` is the platform's native window handle — NSWindow* on macOS,
// HWND on Windows. Returns true if the title bar was actually hidden.
bool hideTitleBar(void* nativeWindow);

// Horizontal space, in logical points, that content at the very top of the
// window must leave clear on the LEADING edge so it does not sit underneath
// the window buttons. Zero where the buttons are not overlaid (or where
// hideTitleBar did nothing), so callers can add it unconditionally.
float titleBarInset(void* nativeWindow);

// Height, in logical points, of the title bar the OS would have drawn. Content
// that replaces it should match this: the window buttons are vertically centred
// in a band of THIS height, so a shorter bar leaves them looking cramped and a
// taller one wastes the space the change was meant to reclaim. Zero where
// hideTitleBar did nothing, so callers can treat it as "no override".
float titleBarHeight(void* nativeWindow);

// ── Replacing what the title bar used to do for free ────────────────────────
// Hiding the bar means the app's own content now covers the strip the window
// manager used to handle. On macOS the content view swallows those events, so
// the native drag and double-click-to-zoom stop working even though the window
// is still fully decorated. Both have to be forwarded back explicitly.

// Start an interactive window move, as if the user had grabbed the title bar.
// Call this from a mouse-down/drag over the region standing in for it. The OS
// takes over from there — snapping, multi-monitor and the drag animation all
// come with it, which is the reason for handing the gesture back rather than
// setting the window position frame by frame.
bool beginWindowDrag(void* nativeWindow);

// The other title-bar gesture: double-click to zoom/maximise. Honours the
// system preference for what a double-click does (zoom, minimise, or nothing)
// where the platform exposes it.
bool toggleWindowZoom(void* nativeWindow);

// Must the APPLICATION draw its own minimise / maximise / close buttons?
//
// This is the one place the platforms genuinely disagree, and it is not a
// detail. On macOS the traffic lights live in the window's chrome and survive
// hiding the title bar, so the app draws nothing and merely leaves room for
// them (`titleBarInset`). On Windows the caption buttons are part of the
// NON-CLIENT area that WM_NCCALCSIZE removes, so hiding the bar deletes them —
// an app that assumed the macOS shape would ship a window with no way to close
// it. False where the OS still provides them.
bool titleBarNeedsCustomButtons();

// Minimise. Only needed where the app draws its own buttons — the OS-provided
// ones already do this — but implemented everywhere it is cheap, so the editor
// does not have to branch on platform to wire a menu item to it.
bool minimizeWindow(void* nativeWindow);

} // namespace platwin
