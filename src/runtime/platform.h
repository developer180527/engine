#pragma once
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
class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual bool init(const PlatformConfig& cfg) = 0;
    virtual void shutdown()                      = 0;

    // Native handle bgfx renders into (NSWindow*/HWND/X11 Window/wl_surface).
    // Null => headless, no renderer.
    virtual void* nativeWindowHandle() const = 0;

    virtual void pollEvents()                       = 0;
    virtual bool shouldClose() const                = 0;
    virtual void requestClose()                     = 0;
    virtual void framebufferSize(int& w, int& h) const = 0;

    // Block up to timeoutSeconds waiting for events (used while minimized
    // to avoid spinning). Default: no wait — fine for headless platforms.
    virtual void waitEvents(double /*timeoutSeconds*/) {}
};
