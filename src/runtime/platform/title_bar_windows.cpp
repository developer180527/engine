// ── title_bar_windows — Win32 implementation of the title-bar seam ───────────
//
// ⚠ UNVERIFIED. Written on macOS, where it cannot be compiled or run. Nothing
// in CI builds a Windows target yet (see docs/process/roadmap.md — the Windows
// port is a tracked standing gap), so treat every line below as a reviewed
// PROPOSAL rather than as working code. The first person to build on Windows
// should expect to fix things here. It is isolated in a Windows-only TU, so it
// cannot affect the macOS or Linux builds either way.
//
// ── Why Windows is not the same job as macOS ─────────────────────────────────
// On macOS the caption buttons live in the window's own chrome, so hiding the
// title bar leaves the traffic lights floating over the content and everything
// keeps working. Windows has no equivalent: the minimise / maximise / close
// buttons are part of the NON-CLIENT area, and the only way to reclaim the
// caption strip is WM_NCCALCSIZE, which removes that area — buttons included.
//
// So on Windows the application MUST draw its own window buttons and hit-test
// them, which is why `titleBarNeedsCustomButtons()` exists and returns true
// here and false on macOS. A UI that assumes the macOS shape would come up on
// Windows with no way to close the window.
#include "runtime/platform/title_bar.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

namespace platwin {
namespace {

// The original window procedure, so the subclass can chain rather than
// replace. GLFW and SDL both do real work in theirs — input, focus, DPI — and
// swallowing any of it is how a "cosmetic" change breaks the keyboard.
WNDPROC g_originalProc = nullptr;
HWND    g_hooked       = nullptr;

int resizeBorderThickness(HWND hwnd) {
    // Per-monitor DPI aware: SM_CXFRAME is scaled for the window's monitor, so
    // a hardcoded 8 is wrong the moment the window moves to a 150% display.
    const int frame  = GetSystemMetrics(SM_CXFRAME);
    const int padded = GetSystemMetrics(SM_CXPADDEDBORDER);
    (void)hwnd;
    return frame + padded;
}

LRESULT CALLBACK hookedProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE: {
        if (!wp) break;                       // wparam FALSE: nothing to adjust
        NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);

        // A MAXIMISED window must still be inset by the frame, or its content
        // spills past the work area and covers the taskbar. This is the single
        // most commonly missed case in custom-chrome implementations.
        if (IsZoomed(hwnd)) {
            const int b = resizeBorderThickness(hwnd);
            p->rgrc[0].left   += b;
            p->rgrc[0].right  -= b;
            p->rgrc[0].bottom -= b;
            p->rgrc[0].top    += b;

            // Respect an auto-hide taskbar: if the client covers the full
            // screen edge the taskbar can no longer be revealed, which strands
            // the user. Leaving one pixel is the documented workaround.
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE)
                p->rgrc[0].bottom -= 1;
            return 0;
        }

        // Restored: keep the left/right/bottom resize frame exactly as Windows
        // sized it and reclaim ONLY the caption strip at the top. Zeroing all
        // four edges (the usual shortcut) is what produces a window with no
        // resizable border and a one-pixel drop shadow.
        p->rgrc[0].top += 0;
        return 0;
    }

    case WM_NCHITTEST: {
        // With the caption gone, Windows no longer reports HTTOP / HTCAPTION,
        // so the top resize edge has to be reconstructed. Everything else is
        // still handled by the default proc, which knows about the other
        // borders and the corners.
        const LRESULT def = CallWindowProc(g_originalProc, hwnd, msg, wp, lp);
        if (def != HTCLIENT) return def;      // a real border or corner

        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < resizeBorderThickness(hwnd)) return HTTOP;

        // Deliberately NOT returning HTCAPTION for the menu-bar strip: the
        // drag is driven from the UI through beginWindowDrag() instead, so the
        // strip stays clickable for menus. Reporting HTCAPTION here would make
        // Windows eat the press and the menus would stop opening.
        return HTCLIENT;
    }

    case WM_DESTROY:
        if (hwnd == g_hooked) { g_hooked = nullptr; g_originalProc = nullptr; }
        break;

    default: break;
    }
    return CallWindowProc(g_originalProc, hwnd, msg, wp, lp);
}

} // namespace

bool hideTitleBar(void* nativeWindow) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (g_hooked == hwnd) return true;                  // already applied
    if (g_hooked) return false;                         // one window only

    g_originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                         reinterpret_cast<LONG_PTR>(hookedProc)));
    if (!g_originalProc) return false;
    g_hooked = hwnd;

    // Keep the drop shadow and the snap/aero behaviours that come with a real
    // frame. Without this the window looks flat and detached from the desktop.
    const MARGINS m{ 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &m);

    // Force the frame to be recalculated now, so WM_NCCALCSIZE runs before the
    // first paint rather than at the first resize.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// Windows puts its caption buttons on the TRAILING edge, so there is nothing
// to clear on the leading side. The app draws its own buttons on the right —
// see titleBarNeedsCustomButtons().
float titleBarInset(void*) { return 0.0f; }

float titleBarHeight(void* nativeWindow) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd || !IsWindow(hwnd)) return 0.0f;
    // What the caption WOULD have been, derived rather than assumed: this is
    // DPI-dependent and differs between Windows 10 and 11.
    const int caption = GetSystemMetrics(SM_CYCAPTION);
    const int frame   = GetSystemMetrics(SM_CYSIZEFRAME);
    const int padded  = GetSystemMetrics(SM_CXPADDEDBORDER);
    const float h = (float)(caption + frame + padded);
    return (h > 0.0f && h < 200.0f) ? h : 0.0f;
}

bool beginWindowDrag(void* nativeWindow) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd || !IsWindow(hwnd)) return false;
    // Hand the gesture to the window manager exactly as a title-bar press
    // would: Aero Snap, multi-monitor and the drag ghost all come with it.
    ReleaseCapture();
    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return true;
}

bool toggleWindowZoom(void* nativeWindow) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd || !IsWindow(hwnd)) return false;
    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    return true;
}

bool titleBarNeedsCustomButtons() { return true; }

bool minimizeWindow(void* nativeWindow) {
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (!hwnd || !IsWindow(hwnd)) return false;
    ShowWindow(hwnd, SW_MINIMIZE);
    return true;
}

} // namespace platwin

#endif // _WIN32
