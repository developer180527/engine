#pragma once
#include "runtime/platform/platform.h"

struct GLFWwindow;
struct GLFWcursor;

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
    // Colour stage C — both backends hand back an NSWindow on macOS, so
    // the one platwin implementation serves both.
    bool  enableHdrOutput() override;
    float hdrHeadroom() const override;
    std::string hdrSurfaceDescription() const override;

    // UI input (ui_input.h). Enabling installs CHAINED callbacks: whatever was
    // registered before (ImGui, window_ops' input sink) still runs.
    void  enableUiInput(bool on) override;
    void  takeUiEvents(std::vector<uiin::Event>& out) override;
    void  windowSize(int& w, int& h) const override;
    std::string clipboardText() const override;
    void  setClipboardText(const std::string& utf8) override;
    void  setUiCursor(uiin::Cursor cursor) override;

    // Tool windows (IToolWindow)
    std::unique_ptr<IToolWindow> createToolWindow(const PlatformConfig& cfg) override;
    void  contentOrigin(float& x, float& y) const override;
    bool  globalPointer(float& x, float& y, bool& primaryDown) const override;

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

public:   // the C callbacks append here; not part of the interface
    // Where one window's UI events go: this platform's own, or a tool
    // window's. The callbacks are shared; the sink is per window.
    struct UiSink {
        bool                     enabled = false;
        std::vector<uiin::Event> events;
        uiin::Modifiers          mods;       // GLFW gives mods on keys/buttons only
        bool                     close = false;
    };
    UiSink                   m_ui;
    bool                     m_uiInstalled = false;
    uiin::Cursor             m_cursor = uiin::Cursor::Arrow;

};
