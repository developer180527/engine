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
    void  pollEvents() override;
    bool  shouldClose() const override;
    void  requestClose() override;
    void  framebufferSize(int& w, int& h) const override;
    void  waitEvents(double timeoutSeconds) override;
    void  setTitle(const std::string& title) override;

    // APP-LAYER ONLY (editor, engine_host, samples — code that explicitly
    // chose this platform). Engine internals must stay behind IPlatform so
    // alternative platforms don't compile-but-break on hidden GLFW deps.
    GLFWwindow* glfwWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};
