// ── window_ops_glfw — GLFW implementation of the editor's window seam ────────
// The one place under src/editor/ that names GLFW (the vendored ImGui platform
// backend aside). See window_ops.h for why this seam exists.
#include "editor/window_ops.h"

#if defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND) || __has_include(<wayland-client.h>)
        #define GLFW_EXPOSE_NATIVE_WAYLAND
    #endif
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// The engine's Key/MouseButton values are numerically identical to GLFW's, and
// input_system.h static_asserts that. This backend may therefore cast directly
// — an SDL3 implementation of this same header will need a translation table
// instead, which is exactly the kind of backend detail this seam contains.
namespace {
inline GLFWwindow* win(edwin::WindowHandle w) {
    return static_cast<GLFWwindow*>(w);
}
} // namespace

namespace edwin {

bool isFocused(WindowHandle w) {
    return w && glfwGetWindowAttrib(win(w), GLFW_FOCUSED);
}

bool isKeyDown(WindowHandle w, Key k) {
    if (!w || k == Key::Unknown) return false;
    return glfwGetKey(win(w), static_cast<int>(k)) == GLFW_PRESS;
}

bool isMouseButtonDown(WindowHandle w, MouseButton b) {
    if (!w) return false;
    return glfwGetMouseButton(win(w), static_cast<int>(b)) == GLFW_PRESS;
}

void cursorPos(WindowHandle w, double& x, double& y) {
    if (!w) { x = 0.0; y = 0.0; return; }
    glfwGetCursorPos(win(w), &x, &y);
}

CursorMode cursorMode(WindowHandle w) {
    if (!w) return CursorMode::Normal;
    return glfwGetInputMode(win(w), GLFW_CURSOR) == GLFW_CURSOR_DISABLED
         ? CursorMode::Captured : CursorMode::Normal;
}

void setCursorMode(WindowHandle w, CursorMode mode) {
    if (!w) return;
    glfwSetInputMode(win(w), GLFW_CURSOR,
        mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void framebufferSize(WindowHandle w, int& fbW, int& fbH) {
    if (!w) { fbW = 0; fbH = 0; return; }
    glfwGetFramebufferSize(win(w), &fbW, &fbH);
}

void requestClose(WindowHandle w) {
    if (w) glfwSetWindowShouldClose(win(w), GLFW_TRUE);
}

void* nativeWindowHandle(WindowHandle w) {
    if (!w) return nullptr;
#if defined(__APPLE__)
    return (void*)glfwGetCocoaWindow(win(w));
#elif defined(_WIN32)
    return (void*)glfwGetWin32Window(win(w));
#elif defined(__linux__)
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandWindow(win(w));
    #endif
    return (void*)(uintptr_t)glfwGetX11Window(win(w));
#else
    return nullptr;
#endif
}

const char* keyName(Key k) {
    if (k == Key::Unknown) return nullptr;
    return glfwGetKeyName(static_cast<int>(k), 0);
}

} // namespace edwin
