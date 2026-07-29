// ── window_ops_sdl3 — SDL3 implementation of the editor's window seam ────────
// The SDL3 counterpart of window_ops_glfw.cpp. See editor/window_ops.h.
//
// NOT YET COMPILED INTO THE EDITOR: building the editor on SDL3 also needs the
// ImGui SDL3 platform backend wired (with event forwarding out of
// Sdl3Platform::pollEvents) and an SDL3 window input source to replace the
// GLFW-callback InputSystem. src/CMakeLists.txt gates the editor accordingly.
// This file exists now because it is the part that proves the seam design —
// and because the key mapping below is the real work, not the boilerplate.
#include "editor/window_ops.h"

#include "runtime/input/sdl3_keymap.h"

#include <SDL3/SDL.h>

namespace {

inline SDL_Window* win(edwin::WindowHandle w) {
    return static_cast<SDL_Window*>(w);
}

// Key -> SDL_Scancode lives in runtime/input/sdl3_keymap.h: the window input
// source needs the same table, and two copies of a keymap silently drift.
using sdl3keys::toScancode;

} // namespace

namespace edwin {

bool isFocused(WindowHandle w) {
    return w && (SDL_GetWindowFlags(win(w)) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool isKeyDown(WindowHandle w, Key k) {
    // SDL keyboard state is process-wide (one queue, not per window), so the
    // handle only decides WHETHER to read: an unfocused window must not report
    // keys, or a detached panel would steal input from the focused one.
    if (!isFocused(w)) return false;
    const SDL_Scancode sc = toScancode(k);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int n = 0;
    const bool* state = SDL_GetKeyboardState(&n);
    return state && (int)sc < n && state[sc];
}

bool isMouseButtonDown(WindowHandle w, MouseButton b) {
    if (!isFocused(w)) return false;
    const SDL_MouseButtonFlags m = SDL_GetMouseState(nullptr, nullptr);
    switch (b) {
        case MouseButton::Left:   return (m & SDL_BUTTON_LMASK) != 0;
        case MouseButton::Right:  return (m & SDL_BUTTON_RMASK) != 0;
        case MouseButton::Middle: return (m & SDL_BUTTON_MMASK) != 0;
    }
    return false;
}

void cursorPos(WindowHandle w, double& x, double& y) {
    if (!w) { x = 0.0; y = 0.0; return; }
    float fx = 0.0f, fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);          // window-relative, like GLFW's
    x = fx; y = fy;
}

CursorMode cursorMode(WindowHandle w) {
    if (!w) return CursorMode::Normal;
    return SDL_GetWindowRelativeMouseMode(win(w)) ? CursorMode::Captured
                                                  : CursorMode::Normal;
}

void setCursorMode(WindowHandle w, CursorMode mode) {
    if (!w) return;
    SDL_SetWindowRelativeMouseMode(win(w), mode == CursorMode::Captured);
}

void framebufferSize(WindowHandle w, int& fbW, int& fbH) {
    if (!w) { fbW = 0; fbH = 0; return; }
    SDL_GetWindowSizeInPixels(win(w), &fbW, &fbH);   // pixels, not points
}

void requestClose(WindowHandle w) {
    if (!w) return;
    // SDL has no per-window "should close" flag; synthesize the event the
    // platform's pump already handles so both paths converge.
    SDL_Event e{};
    e.type            = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = SDL_GetWindowID(win(w));
    SDL_PushEvent(&e);
}

void* nativeWindowHandle(WindowHandle w) {
    if (!w) return nullptr;
    const SDL_PropertiesID props = SDL_GetWindowProperties(win(w));
#if defined(SDL_PLATFORM_MACOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_WIN32)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    if (void* surf = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))
        return surf;
    return (void*)(uintptr_t)SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
#else
    (void)props;
    return nullptr;
#endif
}

const char* keyName(Key k) {
    const SDL_Scancode sc = toScancode(k);
    if (sc == SDL_SCANCODE_UNKNOWN) return nullptr;
    // Layout-aware label for the key at that physical position. SDL returns ""
    // (not null) for keys with no name — normalize to null so callers fall
    // back to their own table, matching the GLFW backend's contract.
    const char* n = SDL_GetKeyName(SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false));
    return (n && *n) ? n : nullptr;
}

} // namespace edwin
