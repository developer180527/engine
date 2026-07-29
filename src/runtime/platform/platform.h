#pragma once
#include <functional>
#include <memory>
#include <string>

struct PlatformConfig {
    std::string title  = "Engine";
    int         width  = 1280;
    int         height = 720;
};

// ── IPlatform ───────────────────────────────────────────────────────────────
// Owns the OS window and the event pump. EngineRuntime talks to the platform
// only through this interface, so SDK consumers can swap implementations:
//
//   GlfwPlatform      — default; creates a GLFW window (what the editor uses)
//   HeadlessPlatform  — no window, no GPU; for dedicated servers and CLI tools
//   (your own)        — embed the engine in an existing native window by
//                       returning its handle from nativeWindowHandle()
//
// nativeWindowHandle() returning null means headless: EngineRuntime skips
// renderer (bgfx) initialization entirely.
enum class CursorMode { Normal, Captured };

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual bool init(const PlatformConfig& cfg) = 0;
    virtual void shutdown()                      = 0;

    // Native handle bgfx renders into (NSWindow*/HWND/X11 Window/wl_surface).
    // Null => headless, no renderer.
    virtual void* nativeWindowHandle() const = 0;

    // The display/server connection the window belongs to — X11 `Display*` or
    // Wayland `wl_display*`, which bgfx needs as PlatformData::ndt. Null on
    // macOS and Windows, where the window handle alone identifies the device;
    // null is therefore the correct default, not "unimplemented".
    virtual void* nativeDisplayHandle() const { return nullptr; }

    // Explicit capability: can this platform host a GPU device? The runtime
    // checks THIS (not the null-handle convention) to decide whether to
    // initialize the renderer. Default derives from the handle so existing
    // platforms keep working; custom platforms may override directly.
    virtual bool supportsRendering() const { return nativeWindowHandle() != nullptr; }

    virtual void pollEvents()                       = 0;
    virtual bool shouldClose() const                = 0;
    virtual void requestClose()                     = 0;
    virtual void framebufferSize(int& w, int& h) const = 0;

    // Block up to timeoutSeconds waiting for events (used while minimized
    // to avoid spinning). Default: no wait — fine for headless platforms.
    virtual void waitEvents(double /*timeoutSeconds*/) {}

    // Update the window title (no-op where there is no window). Used when a
    // project is opened after init — the title follows the project name.
    virtual void setTitle(const std::string& /*title*/) {}

    // Cursor capture for mouse-look. Captured = hidden + locked to the window
    // (raw relative motion via mouseDelta); Normal = visible OS cursor. No-op
    // where there is no window (headless).
    virtual void setCursorMode(CursorMode /*mode*/) {}

    // Observe the backend's native events as they are pumped. `nativeEvent`
    // points at the backend's OWN event type (SDL_Event* on SDL3). Set it
    // before the first pollEvents().
    //
    // This exists because SDL has ONE process-wide event queue: the ImGui
    // platform backend and the window input source both need to SEE events,
    // but only one component may own the pump. So the platform pumps and
    // everyone else observes. GLFW dispatches through per-window callbacks
    // instead and has no event objects, so this is simply never called there.
    using NativeEventHook = std::function<void(const void* nativeEvent)>;
    virtual void setNativeEventHook(NativeEventHook) {}

    // The WINDOWING LIBRARY's window object (GLFWwindow* / SDL_Window*), as
    // opposed to nativeWindowHandle()'s OS-level handle. Opaque on purpose:
    // apps hand it to whichever ImGui platform backend and window-ops
    // implementation are compiled in, without naming GLFW or SDL themselves.
    // Null when there is no window (headless).
    virtual void* backendWindowHandle() const { return nullptr; }
};

// The platform for the backend this build selected (ENGINE_WINDOW_BACKEND).
// Every app constructs its window through here rather than naming a concrete
// class, so flipping the CMake option switches the whole engine over.
// Headless tools keep constructing HeadlessPlatform directly — that is a
// deliberate choice, not a default.
std::unique_ptr<IPlatform> makeDefaultPlatform();

// Human-readable name of the compiled-in window backend ("glfw" / "sdl3"),
// for logs and --version output.
const char* windowBackendName();
