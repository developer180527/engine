// ── title_bar_stub — the platforms where this is not implemented yet ─────────
// Returns false and touches nothing, so the editor comes up with a normal
// system title bar. That is the deliberate fallback: a plain title bar is a
// cosmetic shortfall, whereas the "easy" alternative — an undecorated window —
// silently costs resize, snap, minimise and maximise. See title_bar.h.
#include "runtime/platform/title_bar.h"

namespace platwin {

bool  hideTitleBar(void*)  { return false; }
float titleBarInset(void*)  { return 0.0f; }
float titleBarHeight(void*) { return 0.0f; }
// No hidden title bar here, so the real one still handles both gestures.
bool  beginWindowDrag(void*)   { return false; }
bool  toggleWindowZoom(void*)  { return false; }
// A real title bar is still there, so it still has real buttons.
bool  titleBarNeedsCustomButtons() { return false; }
bool  minimizeWindow(void*)        { return false; }

// No OS fullscreen toggle exists for an ordinary window here (title_bar.h).
bool setUserFullscreenEnabled(void*, bool) { return true; }

} // namespace platwin
