#pragma once
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <vector>
#include <cstring>
#include "runtime/input/input_event.h"

// ── InputSystem ────────────────────────────────────────────────────────────
// Hybrid approach: poll keys/mouse/cursor directly (no callback conflicts
// with ImGui), use callbacks only for scroll + text input which have no
// polling API. Double-buffered state gives correct isPressed/isReleased
// edge detection without touching ImGui's callback chain.
class InputSystem {
public:
    static InputSystem& get() {
        static InputSystem inst;
        return inst;
    }

    // Call once after window creation AND after imguiInit().
    // Only installs scroll + char callbacks (chained to ImGui's).
    void init(GLFWwindow* window) {
        m_window = window;
        // Chain scroll + char — ImGui needs these too
        m_prevScroll = glfwSetScrollCallback(window, cbScroll);
        m_prevChar   = glfwSetCharCallback  (window, cbChar);
        // Seed cursor so first delta is 0
        glfwGetCursorPos(window, &m_lastX, &m_lastY);
    }

    // Call AFTER glfwPollEvents(), before any gameplay code.
    // Polls GLFW state directly — zero callback interference with ImGui.
    void processEvents() {
        if (!m_window) return;

        // ── Double-buffer swap ─────────────────────────────────────────────
        std::memcpy(m_prev,      m_cur,      sizeof(m_cur));
        std::memcpy(m_prevMouse, m_curMouse, sizeof(m_curMouse));

        // ── Poll keys ─────────────────────────────────────────────────────
        // Skip key 0..31 (GLFW_KEY_UNKNOWN etc.), start at Space (32)
        for (int k = 32; k <= GLFW_KEY_LAST; ++k)
            m_cur[k] = (glfwGetKey(m_window, k) == GLFW_PRESS);

        // ── Poll mouse buttons ────────────────────────────────────────────
        for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; ++b)
            m_curMouse[b] = (glfwGetMouseButton(m_window, b) == GLFW_PRESS);

        // ── Cursor delta ──────────────────────────────────────────────────
        double cx, cy;
        glfwGetCursorPos(m_window, &cx, &cy);
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
        for (int k = 32; k <= GLFW_KEY_LAST; ++k)
            if (m_cur[k] && !m_prev[k]) return k;
        return -1;
    }

    // ── Text input ─────────────────────────────────────────────────────────
    const std::vector<uint32_t>& textInput() const { return m_textInput; }

private:
    InputSystem() = default;

    GLFWwindow* m_window = nullptr;

    bool m_cur [GLFW_KEY_LAST + 1] = {};
    bool m_prev[GLFW_KEY_LAST + 1] = {};
    bool m_curMouse [GLFW_MOUSE_BUTTON_LAST + 1] = {};
    bool m_prevMouse[GLFW_MOUSE_BUTTON_LAST + 1] = {};

    float  m_dx = 0, m_dy = 0;
    float  m_cursorX = 0, m_cursorY = 0;
    double m_lastX = 0, m_lastY = 0;

    float  m_scrollX = 0, m_scrollY = 0;
    float  m_pendingScrollX = 0, m_pendingScrollY = 0;

    std::vector<uint32_t> m_textInput;
    std::vector<uint32_t> m_pendingText;

    bool m_uiCaptureKb    = false;
    bool m_uiCaptureMouse = false;

    // Previous callbacks (ImGui's) for scroll + char only
    GLFWscrollfun m_prevScroll = nullptr;
    GLFWcharfun   m_prevChar   = nullptr;

    bool valid (int k) const { return k >= 32 && k <= GLFW_KEY_LAST; }
    bool validM(int b) const { return b >= 0  && b <= GLFW_MOUSE_BUTTON_LAST; }

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
};
