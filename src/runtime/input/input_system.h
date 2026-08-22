#pragma once
#include <vector>
#include <cstring>
#include "runtime/input/input_event.h"
#include "runtime/platform/window_ops.h"

// ── InputSystem ────────────────────────────────────────────────────────────
// The WINDOW input source: keys/mouse/cursor polled from the windowing layer,
// double-buffered so isPressed/isReleased edges are correct. Polling (rather
// than callbacks) is what keeps it out of ImGui's way; only scroll and text
// input have no polling API and so arrive as events.
//
// State is indexed by the ENGINE's code space (Key/MouseButton values), not by
// the backend's, so everything below the poll is backend-independent.
//
// ── This header names NO windowing library, and that is load-bearing ────────
// It used to `#include <GLFW/glfw3.h>` and branch on ENGINE_WINDOW_BACKEND_SDL3
// in six places. Because input.h, input_map.h, input_sources.h, runtime.cpp and
// the editor all include it, that single include linked GLFW into every build
// of engine_runtime — including SDL3 ones, and including a host that supplies
// its own window. A Qt or Rust editor dragged in a windowing library it had no
// use for, which is precisely the vendor-lock the SDK exists to avoid.
//
// Everything backend-specific now lives behind wsi:: in
// runtime/platform/window_ops.h, so each library is reachable from exactly two
// TUs and CMake links only the one selected. Keep it that way: an #include of
// GLFW or SDL here silently re-couples every consumer of this header.
class InputSystem {
public:
    static InputSystem& get() {
        static InputSystem inst;
        return inst;
    }

    // Call once after window creation AND after the UI backend initialises —
    // on GLFW the sink chains to ImGui's scroll/char callbacks, so installing
    // first would let ImGui displace it.
    //
    // Opaque handle for the same reason as setActiveWindow(): no caller needs a
    // windowing-library type to hand its window to the input source.
    void init(void* window) {
        m_window = window;
        wsi::installInputSink(window, wsi::InputSink{
            this,
            [](void* ctx, double dx, double dy) {
                auto* s = static_cast<InputSystem*>(ctx);
                s->m_pendingScrollX += (float)dx;
                s->m_pendingScrollY += (float)dy;
            },
            [](void* ctx, uint32_t cp) {
                static_cast<InputSystem*>(ctx)->m_pendingText.push_back(cp);
            }});
        // Seed the cursor so the first delta is 0.
        wsi::cursorPos(window, m_lastX, m_lastY);
    }

    // Feed one native event (an SDL_Event* on SDL3). Wired by the app to
    // IPlatform::setNativeEventHook; a no-op on GLFW, which uses callbacks.
    // Which of those is true is the seam's business, not this class's.
    void processNativeEvent(const void* nativeEvent) {
        wsi::feedNativeEvent(nativeEvent);
    }

    // Call AFTER the platform has pumped its events, before any gameplay code.
    // Polls window state directly — no callback interference with ImGui.
    void processEvents() {
        if (!m_window) return;

        // ── Double-buffer swap ─────────────────────────────────────────────
        std::memcpy(m_prev,      m_cur,      sizeof(m_cur));
        std::memcpy(m_prevMouse, m_curMouse, sizeof(m_curMouse));

        // One call each, into the engine's own code space. How the backend
        // gets there — casting on GLFW, a scancode table on SDL — is the
        // seam's problem and no longer visible from here.
        wsi::pollKeyboard(m_window, m_cur, kKeyCodeMax + 1);
        wsi::pollMouseButtons(m_window, m_curMouse, kMouseButtonMax + 1);

        double cx = 0.0, cy = 0.0;
        wsi::cursorPos(m_window, cx, cy);
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
    // Takes an OPAQUE handle so no caller needs a windowing-library type.
    void setActiveWindow(void* w) {
        if (!w || w == m_window) return;
        m_window = w;
        wsi::cursorPos(w, m_lastX, m_lastY);
    }

    // Window focus — the InputManager's gate for raw (system-wide) input.
    bool windowFocused() const { return wsi::isFocused(m_window); }

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

    // Opaque: GLFWwindow* or SDL_Window* depending on the backend. This class
    // never learns which.
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

    bool valid (int k) const { return k >= 32 && k <= kKeyCodeMax; }
    bool validM(int b) const { return b >= 0  && b <= kMouseButtonMax; }
};
