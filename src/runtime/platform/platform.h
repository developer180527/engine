#pragma once
#include "runtime/platform/ui_input.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PlatformConfig {
    std::string title  = "Engine";
    int         width  = 1280;
    int         height = 720;
    // Suppress the OS title bar's DRAWING while keeping the window a normal,
    // fully decorated OS window — resize, snap, minimise, maximise and the
    // window buttons all keep working. This is NOT an undecorated window; see
    // runtime/platform/title_bar.h for why that distinction is the whole
    // point. Where the platform has no implementation yet the flag is ignored
    // and a normal title bar is drawn.
    bool        hideTitleBar = false;
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

// ── IToolWindow — a secondary OS window for tools ────────────────────────────
// A torn-off panel is its own OS window. Games never need one; an editor does,
// and before this only ImGui could make them (through ImGui's own backends), so
// any other GUI had to create windows with GLFW or SDL directly — the coupling
// IPlatform exists to prevent, and a port to a new OS meant porting each GUI.
//
// Created by the platform, so it shares the platform's event pump: SDL has one
// process-wide queue, and the platform routes each event to the window it
// belongs to. UI events are in THIS window's points. Positions are in desktop
// points (the unit GLFW and SDL both use for window placement), the same space
// as IPlatform::globalPointer and contentOrigin.
class IToolWindow {
public:
    virtual ~IToolWindow() = default;
    virtual void* nativeWindowHandle() const = 0;      // what bgfx renders into
    virtual void* nativeDisplayHandle() const { return nullptr; }
    virtual void  windowSize(int& w, int& h) const = 0;       // points
    virtual void  framebufferSize(int& w, int& h) const = 0;  // pixels
    // Top-left of the CLIENT area (not the frame) in desktop points.
    virtual void  contentOrigin(float& x, float& y) const = 0;
    virtual void  setContentOrigin(float x, float y) = 0;
    virtual void  setVisible(bool visible) = 0;
    virtual bool  closeRequested() const = 0;
    virtual void  takeUiEvents(std::vector<uiin::Event>& out) = 0;
    virtual void  setUiCursor(uiin::Cursor /*cursor*/) {}
    virtual void  setTextInput(bool /*active*/, float, float, float, float) {}
};

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

    // ── HDR output (colour pipeline stage C) ────────────────────────────────
    // Configure this window's surface for extended-range output, returning true
    // only if it will now accept linear values above 1.0. Called BEFORE renderer
    // init, because the surface must be right before the device is created.
    // Default false: a platform with no implementation stays SDR, which is the
    // behaviour every platform had before.
    virtual bool enableHdrOutput() { return false; }

    // Peak white in SDR-WHITE UNITS (1 = SDR). Read PER FRAME: on Apple's EDR
    // this moves with the user's brightness slider.
    virtual float hdrHeadroom() const { return 1.0f; }

    // What the surface actually is, for the log — so "HDR is on" can be an
    // observation rather than a claim.
    virtual std::string hdrSurfaceDescription() const { return {}; }

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

    // Called while the user drags a window edge. On macOS the OS runs its own
    // modal loop for a live resize and the app's loop is blocked inside
    // pollEvents until the mouse is released, so without this the old frame
    // is stretched and the layout only catches up afterwards. The hook should
    // draw one frame WITHOUT polling events (EngineRuntime::frameBegin(dt,
    // false)). It runs inside pollEvents, on the main thread. Default: never
    // called, and a platform that does not block during resize needs nothing.
    using LiveResizeHook = std::function<void()>;
    virtual void setLiveResizeHook(LiveResizeHook) {}

    // The WINDOWING LIBRARY's window object (GLFWwindow* / SDL_Window*), as
    // opposed to nativeWindowHandle()'s OS-level handle. Opaque on purpose:
    // apps hand it to whichever ImGui platform backend and window-ops
    // implementation are compiled in, without naming GLFW or SDL themselves.
    // Null when there is no window (headless).
    virtual void* backendWindowHandle() const { return nullptr; }

    // ── UI input (runtime/platform/ui_input.h) ──────────────────────────────
    // For a GUI toolkit that is not ImGui (ImGui still uses its own backends).
    // OFF until enabled, so a game or the ImGui editor pays nothing and sees no
    // change. Once on, each pollEvents() appends to a queue the GUI drains.
    // The defaults are "no UI input": a platform without an implementation
    // (headless, a new port not done yet) simply delivers nothing.
    virtual void enableUiInput(bool /*on*/) {}
    // Move the queued events into `out` (appended) and clear the queue.
    virtual void takeUiEvents(std::vector<uiin::Event>& /*out*/) {}
    // Logical window size (points), which is what UI coordinates are in —
    // framebufferSize() is PIXELS. Their ratio is the content scale.
    virtual void windowSize(int& w, int& h) const { framebufferSize(w, h); }
    virtual std::string clipboardText() const { return {}; }
    virtual void setClipboardText(const std::string& /*utf8*/) {}
    virtual void setUiCursor(uiin::Cursor /*cursor*/) {}
    // Text entry: while active the OS may show an on-screen keyboard or an IME
    // candidate window, placed at this rect (logical units).
    virtual void setTextInput(bool /*active*/, float /*x*/, float /*y*/,
                              float /*w*/, float /*h*/) {}
    virtual uiin::KeyboardConvention keyboardConvention() const {
        return uiin::defaultKeyboardConvention();
    }

    // ── Tool windows (see IToolWindow) ──────────────────────────────────────
    // Null where the platform cannot make more windows (headless, a console,
    // a phone): a GUI then keeps floating panels inside the main window.
    // `cfg.width/height` are the client size in points.
    virtual std::unique_ptr<IToolWindow> createToolWindow(const PlatformConfig& /*cfg*/) {
        return nullptr;
    }
    // The main window's client top-left, in desktop points.
    virtual void contentOrigin(float& x, float& y) const { x = 0; y = 0; }
    // Where the pointer is on the DESKTOP, in points, and whether the primary
    // button is down — a panel dragged between windows is in no one window's
    // space. False where the platform cannot say.
    virtual bool globalPointer(float& /*x*/, float& /*y*/, bool& /*primaryDown*/) const {
        return false;
    }
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
