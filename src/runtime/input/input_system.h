#pragma once
#include <vector>
#include <cstring>
#include "runtime/input/input_event.h"

#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    // Scroll and text arrive as EVENTS here (there is no SDL polling API for
    // either), fed in by the platform's native-event hook — see
    // processNativeEvent(). Key/mouse/cursor are still polled.
    #include "runtime/input/sdl3_keymap.h"
#else
    #define GLFW_INCLUDE_NONE
    #include <GLFW/glfw3.h>

    // The engine-owned Key/MouseButton constants (input_event.h, backend-free)
    // must stay numerically identical to GLFW's — this backend casts between
    // them, and kKeyCodeMax/kMouseButtonMax mirror GLFW's bounds.
    static_assert((int)Key::Space      == GLFW_KEY_SPACE);
    static_assert((int)Key::Escape     == GLFW_KEY_ESCAPE);
    static_assert((int)Key::Enter      == GLFW_KEY_ENTER);
    static_assert((int)Key::W          == GLFW_KEY_W);
    static_assert((int)Key::Num9       == GLFW_KEY_9);
    static_assert((int)Key::F12        == GLFW_KEY_F12);
    static_assert((int)Key::LeftShift  == GLFW_KEY_LEFT_SHIFT);
    static_assert((int)Key::RightSuper == GLFW_KEY_RIGHT_SUPER);
    static_assert((int)MouseButton::Left   == GLFW_MOUSE_BUTTON_LEFT);
    static_assert((int)MouseButton::Middle == GLFW_MOUSE_BUTTON_MIDDLE);
    static_assert(kKeyCodeMax     == GLFW_KEY_LAST);
    static_assert(kMouseButtonMax == GLFW_MOUSE_BUTTON_LAST);
#endif

// ── InputSystem ────────────────────────────────────────────────────────────
// The WINDOW input source: keys/mouse/cursor polled from the windowing layer,
// double-buffered so isPressed/isReleased edges are correct. Polling (rather
// than callbacks) is what keeps it out of ImGui's way; only scroll and text
// input have no polling API and so arrive as events.
//
// State is indexed by the ENGINE's code space (Key/MouseButton values), not by
// the backend's, so everything below the poll is backend-independent.
class InputSystem {
public:
    static InputSystem& get() {
        static InputSystem inst;
        return inst;
    }

    // Call once after window creation AND after imguiInit().
    // Only installs scroll + char callbacks (chained to ImGui's).
    // Opaque handle for the same reason as setActiveWindow(): the editor must
    // not need GLFW types to hand its window to the window input source.
    void init(void* window) {
        m_window = window;
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
        // No callbacks to install: SDL has one event queue and the platform
        // hook feeds us from it (processNativeEvent). Seed the cursor so the
        // first delta is 0.
        float mx = 0.0f, my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        m_lastX = mx; m_lastY = my;
#else
        auto* gw = static_cast<GLFWwindow*>(window);
        // Chain scroll + char — ImGui needs these too
        m_prevScroll = glfwSetScrollCallback(gw, cbScroll);
        m_prevChar   = glfwSetCharCallback  (gw, cbChar);
        // Seed cursor so first delta is 0
        glfwGetCursorPos(gw, &m_lastX, &m_lastY);
#endif
    }

    // Feed one native event (SDL_Event* on SDL3). Wired by the app to
    // IPlatform::setNativeEventHook. No-op on GLFW, which uses callbacks.
    void processNativeEvent(const void* nativeEvent) {
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
        const SDL_Event* e = static_cast<const SDL_Event*>(nativeEvent);
        if (!e) return;
        if (e->type == SDL_EVENT_MOUSE_WHEEL) {
            m_pendingScrollX += e->wheel.x;
            m_pendingScrollY += e->wheel.y;
        } else if (e->type == SDL_EVENT_TEXT_INPUT && e->text.text) {
            // SDL delivers UTF-8; the engine's text stream is codepoints
            // (GLFW's char callback already gave codepoints).
            appendUtf8AsCodepoints(e->text.text);
        }
#else
        (void)nativeEvent;
#endif
    }

    // Call AFTER glfwPollEvents(), before any gameplay code.
    // Polls GLFW state directly — zero callback interference with ImGui.
    void processEvents() {
        if (!m_window) return;

        // ── Double-buffer swap ─────────────────────────────────────────────
        std::memcpy(m_prev,      m_cur,      sizeof(m_cur));
        std::memcpy(m_prevMouse, m_curMouse, sizeof(m_curMouse));

#if defined(ENGINE_WINDOW_BACKEND_SDL3)
        // ── Poll keys ─────────────────────────────────────────────────────
        // SDL's state array is indexed by SCANCODE, so we walk the engine's
        // key list and translate, rather than sweeping our own code space.
        int numKeys = 0;
        const bool* ks = SDL_GetKeyboardState(&numKeys);
        std::memset(m_cur, 0, sizeof(m_cur));
        if (ks) {
            size_t nk = 0;
            const Key* keys = sdl3keys::allKeys(nk);
            for (size_t i = 0; i < nk; ++i) {
                const SDL_Scancode sc = sdl3keys::toScancode(keys[i]);
                if (sc != SDL_SCANCODE_UNKNOWN && (int)sc < numKeys && ks[sc])
                    m_cur[(int)keys[i]] = true;
            }
        }

        // ── Poll mouse buttons + cursor ───────────────────────────────────
        float fx = 0.0f, fy = 0.0f;
        const SDL_MouseButtonFlags mb = SDL_GetMouseState(&fx, &fy);
        std::memset(m_curMouse, 0, sizeof(m_curMouse));
        m_curMouse[(int)MouseButton::Left]   = (mb & SDL_BUTTON_LMASK) != 0;
        m_curMouse[(int)MouseButton::Right]  = (mb & SDL_BUTTON_RMASK) != 0;
        m_curMouse[(int)MouseButton::Middle] = (mb & SDL_BUTTON_MMASK) != 0;

        double cx = fx, cy = fy;
#else
        // ── Poll keys ─────────────────────────────────────────────────────
        // Skip key 0..31 (GLFW_KEY_UNKNOWN etc.), start at Space (32)
        auto* gw = static_cast<GLFWwindow*>(m_window);
        for (int k = 32; k <= kKeyCodeMax; ++k)
            m_cur[k] = (glfwGetKey(gw, k) == GLFW_PRESS);

        // ── Poll mouse buttons ────────────────────────────────────────────
        for (int b = 0; b <= kMouseButtonMax; ++b)
            m_curMouse[b] = (glfwGetMouseButton(gw, b) == GLFW_PRESS);

        // ── Cursor delta ──────────────────────────────────────────────────
        double cx, cy;
        glfwGetCursorPos(gw, &cx, &cy);
#endif
        m_dx      = (float)(cx - m_lastX);
        m_dy      = (float)(cy - m_lastY);
        m_lastX   = cx; m_lastY = cy;
        m_cursorX = (float)cx;
        m_cursorY = (float)cy;

        // ── Flush callback-driven accumulators ────────────────────────────
        m_scrollX = m_pendingScrollX; m_pendingScrollX = 0.0f;
        m_scrollY = m_pendingScrollY; m_pendingScrollY = 0.0f;
        m_textInput = std::move(m_pendingText);
        m_pendingText.clear();
    }

    // ── Key queries ────────────────────────────────────────────────────────
    bool isKeyDown    (int k) const { return !m_uiCaptureKb && valid(k) && m_cur[k]; }
    bool isKeyPressed (int k) const { return !m_uiCaptureKb && valid(k) &&  m_cur[k] && !m_prev[k]; }
    bool isKeyReleased(int k) const { return !m_uiCaptureKb && valid(k) && !m_cur[k] &&  m_prev[k]; }

    // ── Mouse queries ──────────────────────────────────────────────────────
    bool isMouseDown    (int b) const { return !m_uiCaptureMouse && validM(b) && m_curMouse[b]; }
    bool isMousePressed (int b) const { return !m_uiCaptureMouse && validM(b) &&  m_curMouse[b] && !m_prevMouse[b]; }
    bool isMouseReleased(int b) const { return !m_uiCaptureMouse && validM(b) && !m_curMouse[b] &&  m_prevMouse[b]; }

    float mouseDeltaX()  const { return m_dx; }
    float mouseDeltaY()  const { return m_dy; }
    float scrollDeltaX() const { return m_scrollX; }
    float scrollDeltaY() const { return m_scrollY; }
    float cursorX()      const { return m_cursorX; }
    float cursorY()      const { return m_cursorY; }

    // Retarget polling to another OS window (editor: a detached Game View
    // lives in its own OS window — keys/cursor must be read THERE). Reseeds
    // the cursor baseline so the switch doesn't produce a delta spike.
    //
    // Takes an OPAQUE handle so callers (the editor) don't need GLFW types:
    // this backend knows it is really a GLFWwindow*, they don't. The window
    // source is GLFW-backed by design — GLFW keeps window + text/IME — but
    // that must not leak into the editor's own headers.
    void setActiveWindow(void* w) {
        if (!w || w == m_window) return;
        m_window = w;
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
        float mx = 0.0f, my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        m_lastX = mx; m_lastY = my;
#else
        glfwGetCursorPos(static_cast<GLFWwindow*>(w), &m_lastX, &m_lastY);
#endif
    }

    // Window focus — the InputManager's gate for raw (system-wide) input.
    bool windowFocused() const {
        if (!m_window) return false;
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
        return (SDL_GetWindowFlags(static_cast<SDL_Window*>(m_window))
                & SDL_WINDOW_INPUT_FOCUS) != 0;
#else
        return glfwGetWindowAttrib(static_cast<GLFWwindow*>(m_window),
                                   GLFW_FOCUSED) != 0;
#endif
    }

    // ── UI focus gate ──────────────────────────────────────────────────────
    void setUICapture(bool keyboard, bool mouse) noexcept {
        m_uiCaptureKb    = keyboard;
        m_uiCaptureMouse = mouse;
    }
    bool uiCapturesKeyboard() const noexcept { return m_uiCaptureKb; }
    bool uiCapturesMouse()    const noexcept { return m_uiCaptureMouse; }

    // ── Key capture (settings UI rebinding) ───────────────────────────────
    // Raw — bypasses UI gate. Only use in key-capture context.
    int anyKeyPressedRaw() const {
        for (int k = 32; k <= kKeyCodeMax; ++k)
            if (m_cur[k] && !m_prev[k]) return k;
        return -1;
    }

    // ── Text input ─────────────────────────────────────────────────────────
    const std::vector<uint32_t>& textInput() const { return m_textInput; }

private:
    InputSystem() = default;

    // Opaque: GLFWwindow* or SDL_Window* depending on the backend.
    void* m_window = nullptr;

    bool m_cur [kKeyCodeMax + 1] = {};
    bool m_prev[kKeyCodeMax + 1] = {};
    bool m_curMouse [kMouseButtonMax + 1] = {};
    bool m_prevMouse[kMouseButtonMax + 1] = {};

    float  m_dx = 0, m_dy = 0;
    float  m_cursorX = 0, m_cursorY = 0;
    double m_lastX = 0, m_lastY = 0;

    float  m_scrollX = 0, m_scrollY = 0;
    float  m_pendingScrollX = 0, m_pendingScrollY = 0;

    std::vector<uint32_t> m_textInput;
    std::vector<uint32_t> m_pendingText;

    bool m_uiCaptureKb    = false;
    bool m_uiCaptureMouse = false;

#if !defined(ENGINE_WINDOW_BACKEND_SDL3)
    // Previous callbacks (ImGui's) for scroll + char only
    GLFWscrollfun m_prevScroll = nullptr;
    GLFWcharfun   m_prevChar   = nullptr;
#endif

    bool valid (int k) const { return k >= 32 && k <= kKeyCodeMax; }
    bool validM(int b) const { return b >= 0  && b <= kMouseButtonMax; }

#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    // Minimal UTF-8 -> codepoint decode for SDL_EVENT_TEXT_INPUT. Malformed
    // sequences are skipped rather than emitted as replacement characters: a
    // text field should drop garbage, not insert it.
    void appendUtf8AsCodepoints(const char* utf8) {
        const unsigned char* p = (const unsigned char*)utf8;
        while (*p) {
            uint32_t cp = 0; int extra = 0;
            if      (*p < 0x80) { cp = *p;        extra = 0; }
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
            if (ok && cp) m_pendingText.push_back(cp);
        }
    }
#else
    // ── Callbacks (scroll + char only) ────────────────────────────────────
    static void cbScroll(GLFWwindow* w, double x, double y) {
        auto& s = get();
        s.m_pendingScrollX += (float)x;
        s.m_pendingScrollY += (float)y;
        if (s.m_prevScroll) s.m_prevScroll(w, x, y); // chain to ImGui
    }
    static void cbChar(GLFWwindow* w, unsigned int cp) {
        auto& s = get();
        s.m_pendingText.push_back(cp);
        if (s.m_prevChar) s.m_prevChar(w, cp); // chain to ImGui
    }
#endif
};
