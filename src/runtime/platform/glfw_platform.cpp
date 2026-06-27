#include "runtime/platform/glfw_platform.h"

#include <cstdio>

// Platform-specific native window handle for bgfx
#if defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    // Wayland first, X11 fallback — matches modern distro defaults.
    // GLFW 3.4+ exposes the Wayland surface via glfwGetWaylandWindow();
    // older GLFW only has X11. Both defines are harmless if the backend
    // isn't present: we pick at runtime below.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND) || __has_include(<wayland-client.h>)
        #define GLFW_EXPOSE_NATIVE_WAYLAND
    #endif
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

bool GlfwPlatform::init(const PlatformConfig& cfg) {
    if (!glfwInit()) {
        std::printf("[Platform] GLFW init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(cfg.width, cfg.height,
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::printf("[Platform] Window creation failed\n");
        return false;
    }
    return true;
}

void GlfwPlatform::shutdown() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}

void* GlfwPlatform::nativeWindowHandle() const {
#if defined(__APPLE__)
    return glfwGetCocoaWindow(m_window);
#elif defined(_WIN32)
    return glfwGetWin32Window(m_window);
#elif defined(__linux__)
    // Prefer Wayland when available (GLFW 3.4+); fall back to X11.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandWindow(m_window);
    #endif
    return (void*)glfwGetX11Window(m_window);
#else
    #error "Unsupported platform — add native window handle retrieval"
#endif
}

void GlfwPlatform::pollEvents() {
    glfwPollEvents();
}

bool GlfwPlatform::shouldClose() const {
    return m_window && glfwWindowShouldClose(m_window);
}

void GlfwPlatform::requestClose() {
    if (m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void GlfwPlatform::framebufferSize(int& w, int& h) const {
    if (m_window) glfwGetFramebufferSize(m_window, &w, &h);
    else { w = 0; h = 0; }
}

void GlfwPlatform::waitEvents(double timeoutSeconds) {
    glfwWaitEventsTimeout(timeoutSeconds);
}

void GlfwPlatform::setTitle(const std::string& title) {
    if (m_window) glfwSetWindowTitle(m_window, title.c_str());
}

void GlfwPlatform::setCursorMode(CursorMode mode) {
    if (!m_window) return;
    glfwSetInputMode(m_window, GLFW_CURSOR,
        mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}
