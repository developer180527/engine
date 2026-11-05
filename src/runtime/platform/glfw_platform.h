#pragma once
#include "runtime/platform/platform.h"

struct GLFWwindow;
struct GLFWmonitor;
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

    // ── Main-window control (WO-070) ────────────────────────────────────────
    bool windowControl() const override { return true; }
    bool canExclusiveFullscreen() const override;
    bool canPositionWindow() const override;
    std::vector<MonitorDesc> monitors() override;
    std::vector<DisplayModeDesc> monitorModes(uint32_t monitorId) override;
    bool windowState(WindowState& out) const override;
    bool setWindowMode(WindowMode mode, uint32_t monitorId,
                       const DisplayModeDesc* exclusive) override;
    bool setWindowSize(int w, int h) override;
    bool setWindowPosition(int x, int y) override;
    bool setWindowSizeLimits(int minW, int minH, int maxW, int maxH) override;
    bool setWindowAspect(int numerator, int denominator) override;
    bool setWindowFlags(uint32_t flags) override;
    bool showWindow(bool visible) override;
    bool focusWindow() override;
    void takeWindowEvents(std::vector<WindowEvent>& out) override;

private:
    GLFWwindow* m_window = nullptr;

    // Window control state (WO-070).
    GLFWmonitor* monitorById(uint32_t id);
    uint32_t idOf(GLFWmonitor* m);
    void     gatherWindowEvents();
    std::vector<GLFWmonitor*> m_monitorIds;  // id = index + 1, append-only
    WindowMode  m_mode = WindowMode::Windowed;
    int         m_minW = 0, m_minH = 0, m_maxW = 0, m_maxH = 0;   // 0: no limit
    int         m_windowedX = 0, m_windowedY = 0, m_windowedW = 0, m_windowedH = 0;
    WindowState m_lastState;
    bool        m_haveLastState = false;
    bool        m_closedReported = false;
public:   // the close callback reads these
    bool        m_handleClose = false;
    std::vector<WindowEvent> m_windowEvents;
    void (*m_prevCloseCallback)(GLFWwindow*) = nullptr;
private:

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
