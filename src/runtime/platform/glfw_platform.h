#pragma once
#include "runtime/platform/platform.h"

struct GLFWwindow;

// Default IPlatform: creates and owns a GLFW window. The editor (and any
// game that wants a stock OS window) uses this. Callers that need raw GLFW
// access — ImGui glue, input callbacks — keep a pointer via glfwWindow().
class GlfwPlatform final : public IPlatform {
public:
    bool init(const PlatformConfig& cfg) override;
    void shutdown() override;

    void* nativeWindowHandle() const override;
    void* nativeDisplayHandle() const override;
    void  pollEvents() override;
    bool  shouldClose() const override;
    void  requestClose() override;
    void  framebufferSize(int& w, int& h) const override;
    void  waitEvents(double timeoutSeconds) override;
    void  setTitle(const std::string& title) override;
    void  setCursorMode(CursorMode mode) override;

    // Opaque GLFWwindow* for the ImGui platform backend and the editor's
    // window-ops implementation — neither of which should have to name a
    // concrete platform class. Prefer this over glfwWindow().
    void* backendWindowHandle() const override { return m_window; }

    // APP-LAYER ONLY, and only for code that genuinely needs the GLFW type
    // (nothing does since the window-ops seam landed — kept for out-of-tree
    // consumers). Engine internals must stay behind IPlatform so alternative
    // platforms don't compile-but-break on hidden GLFW deps.
    GLFWwindow* glfwWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};
