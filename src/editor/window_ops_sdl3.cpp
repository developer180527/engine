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

#include <SDL3/SDL.h>

namespace {

inline SDL_Window* win(edwin::WindowHandle w) {
    return static_cast<SDL_Window*>(w);
}

// ── Key -> SDL_Scancode ─────────────────────────────────────────────────────
// The engine's Key values are GLFW's numbers, so the GLFW backend can cast.
// SDL uses its own enumeration, so this backend must TRANSLATE — and it maps
// to SCANCODES (physical position), not keycodes: that matches what
// glfwGetKey() reports and keeps WASD on the same physical keys under AZERTY
// or Dvorak. Mapping to keycodes instead would silently move movement keys for
// non-QWERTY users, which is the classic version of this bug.
SDL_Scancode toScancode(Key k) {
    switch (k) {
        case Key::Space:      return SDL_SCANCODE_SPACE;
        case Key::Escape:     return SDL_SCANCODE_ESCAPE;
        case Key::Enter:      return SDL_SCANCODE_RETURN;
        case Key::Tab:        return SDL_SCANCODE_TAB;
        case Key::Backspace:  return SDL_SCANCODE_BACKSPACE;
        case Key::Delete:     return SDL_SCANCODE_DELETE;
        case Key::Right:      return SDL_SCANCODE_RIGHT;
        case Key::Left:       return SDL_SCANCODE_LEFT;
        case Key::Down:       return SDL_SCANCODE_DOWN;
        case Key::Up:         return SDL_SCANCODE_UP;

        case Key::A: return SDL_SCANCODE_A;  case Key::B: return SDL_SCANCODE_B;
        case Key::C: return SDL_SCANCODE_C;  case Key::D: return SDL_SCANCODE_D;
        case Key::E: return SDL_SCANCODE_E;  case Key::F: return SDL_SCANCODE_F;
        case Key::G: return SDL_SCANCODE_G;  case Key::H: return SDL_SCANCODE_H;
        case Key::I: return SDL_SCANCODE_I;  case Key::J: return SDL_SCANCODE_J;
        case Key::K: return SDL_SCANCODE_K;  case Key::L: return SDL_SCANCODE_L;
        case Key::M: return SDL_SCANCODE_M;  case Key::N: return SDL_SCANCODE_N;
        case Key::O: return SDL_SCANCODE_O;  case Key::P: return SDL_SCANCODE_P;
        case Key::Q: return SDL_SCANCODE_Q;  case Key::R: return SDL_SCANCODE_R;
        case Key::S: return SDL_SCANCODE_S;  case Key::T: return SDL_SCANCODE_T;
        case Key::U: return SDL_SCANCODE_U;  case Key::V: return SDL_SCANCODE_V;
        case Key::W: return SDL_SCANCODE_W;  case Key::X: return SDL_SCANCODE_X;
        case Key::Y: return SDL_SCANCODE_Y;  case Key::Z: return SDL_SCANCODE_Z;

        // Number ROW (not the keypad) — SDL orders these 1..9 then 0.
        case Key::Num0: return SDL_SCANCODE_0;
        case Key::Num1: return SDL_SCANCODE_1;
        case Key::Num2: return SDL_SCANCODE_2;
        case Key::Num3: return SDL_SCANCODE_3;
        case Key::Num4: return SDL_SCANCODE_4;
        case Key::Num5: return SDL_SCANCODE_5;
        case Key::Num6: return SDL_SCANCODE_6;
        case Key::Num7: return SDL_SCANCODE_7;
        case Key::Num8: return SDL_SCANCODE_8;
        case Key::Num9: return SDL_SCANCODE_9;

        case Key::F1:  return SDL_SCANCODE_F1;   case Key::F2:  return SDL_SCANCODE_F2;
        case Key::F3:  return SDL_SCANCODE_F3;   case Key::F4:  return SDL_SCANCODE_F4;
        case Key::F5:  return SDL_SCANCODE_F5;   case Key::F6:  return SDL_SCANCODE_F6;
        case Key::F7:  return SDL_SCANCODE_F7;   case Key::F8:  return SDL_SCANCODE_F8;
        case Key::F9:  return SDL_SCANCODE_F9;   case Key::F10: return SDL_SCANCODE_F10;
        case Key::F11: return SDL_SCANCODE_F11;  case Key::F12: return SDL_SCANCODE_F12;

        case Key::LeftShift:  return SDL_SCANCODE_LSHIFT;
        case Key::LeftCtrl:   return SDL_SCANCODE_LCTRL;
        case Key::LeftAlt:    return SDL_SCANCODE_LALT;
        case Key::LeftSuper:  return SDL_SCANCODE_LGUI;
        case Key::RightShift: return SDL_SCANCODE_RSHIFT;
        case Key::RightCtrl:  return SDL_SCANCODE_RCTRL;
        case Key::RightAlt:   return SDL_SCANCODE_RALT;
        case Key::RightSuper: return SDL_SCANCODE_RGUI;

        case Key::Unknown:
        default:              return SDL_SCANCODE_UNKNOWN;
    }
}

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
