// ── window_ops_sdl3 — SDL3 implementation of the per-window seam ─────────────
// The SDL3 counterpart of window_ops_glfw.cpp. See window_ops.h.
//
// Compiled whenever ENGINE_WINDOW_BACKEND=sdl3. Building the EDITOR on SDL3
// additionally needs the ImGui SDL3 platform backend wired, with event
// forwarding out of Sdl3Platform::pollEvents; src/CMakeLists.txt gates the
// editor accordingly. The runtime itself is complete on SDL3.
//
// The key mapping below is the real work here, not the boilerplate: SDL indexes
// keyboard state by SCANCODE while the engine's code space mirrors GLFW's, so
// this backend translates where the GLFW one casts.
#include "runtime/platform/window_ops.h"

#include "runtime/input/sdl3_keymap.h"

#include <SDL3/SDL.h>

namespace {

inline SDL_Window* win(wsi::WindowHandle w) {
    return static_cast<SDL_Window*>(w);
}

// Key -> SDL_Scancode lives in runtime/input/sdl3_keymap.h. It used to be
// included by input_system.h too — which is what pulled <SDL3/SDL.h> into every
// TU that touched input. Now this is the only place it reaches.
using sdl3keys::toScancode;

wsi::InputSink g_sink{};

// SDL delivers text as UTF-8; the engine's text stream is codepoints, because
// GLFW's char callback provides those and no consumer should have to know which
// backend it is behind. Malformed sequences are SKIPPED rather than emitted as
// replacement characters: a text field should drop garbage, not insert it.
void emitUtf8AsCodepoints(const char* utf8) {
    if (!utf8 || !g_sink.onText) return;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p) {
        uint32_t cp = 0; int extra = 0;
        if      (*p < 0x80)         { cp = *p;        extra = 0; }
        else if ((*p >> 5) == 0x6)  { cp = *p & 0x1F; extra = 1; }
        else if ((*p >> 4) == 0xE)  { cp = *p & 0x0F; extra = 2; }
        else if ((*p >> 3) == 0x1E) { cp = *p & 0x07; extra = 3; }
        else { ++p; continue; }                     // invalid lead byte
        ++p;
        bool ok = true;
        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (ok && cp) g_sink.onText(g_sink.ctx, cp);
    }
}

} // namespace

namespace wsi {

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

void pollKeyboard(WindowHandle w, bool* out, int count) {
    if (!out || count <= 0) return;
    for (int i = 0; i < count; ++i) out[i] = false;
    // Focus decides whether to read at all: SDL's keyboard state is
    // process-wide, so an unfocused window reporting keys would let a detached
    // panel steal input from the focused one.
    if (!isFocused(w)) return;
    int numKeys = 0;
    const bool* ks = SDL_GetKeyboardState(&numKeys);
    if (!ks) return;
    // Walk the ENGINE's key list and translate, rather than sweeping our own
    // code space: SDL's array is indexed by scancode, which is a different
    // space entirely.
    size_t nk = 0;
    const Key* keys = sdl3keys::allKeys(nk);
    for (size_t i = 0; i < nk; ++i) {
        const int idx = (int)keys[i];
        if (idx < 0 || idx >= count) continue;
        const SDL_Scancode sc = toScancode(keys[i]);
        if (sc != SDL_SCANCODE_UNKNOWN && (int)sc < numKeys && ks[sc])
            out[idx] = true;
    }
}

void pollMouseButtons(WindowHandle w, bool* out, int count) {
    if (!out || count <= 0) return;
    for (int i = 0; i < count; ++i) out[i] = false;
    if (!isFocused(w)) return;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
    auto set = [&](MouseButton b, bool v) {
        const int i = (int)b;
        if (i >= 0 && i < count) out[i] = v;
    };
    set(MouseButton::Left,   (mb & SDL_BUTTON_LMASK) != 0);
    set(MouseButton::Right,  (mb & SDL_BUTTON_RMASK) != 0);
    set(MouseButton::Middle, (mb & SDL_BUTTON_MMASK) != 0);
}

void installInputSink(WindowHandle /*w*/, InputSink sink) {
    // Nothing to chain: SDL has one event queue, and events reach us through
    // feedNativeEvent rather than by displacing another library's callback.
    g_sink = sink;
}

void feedNativeEvent(const void* nativeEvent) {
    const SDL_Event* e = static_cast<const SDL_Event*>(nativeEvent);
    if (!e) return;
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        if (g_sink.onScroll) g_sink.onScroll(g_sink.ctx, e->wheel.x, e->wheel.y);
    } else if (e->type == SDL_EVENT_TEXT_INPUT) {
        emitUtf8AsCodepoints(e->text.text);
    }
}

} // namespace wsi
