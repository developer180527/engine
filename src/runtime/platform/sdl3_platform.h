#pragma once
#include "runtime/platform/platform.h"

#include <unordered_map>

struct SDL_Window;
struct SDL_Cursor;
union  SDL_Event;
class  Sdl3ToolWindow;

// ── Sdl3Platform ─────────────────────────────────────────────────────────────
// IPlatform on SDL3. Selected with -DENGINE_WINDOW_BACKEND=sdl3; GLFW remains
// the default while this is brought up, so both backends stay compiling and
// `main` is never broken by the migration.
//
// WHY SDL3 at all: it is the window + raw-input PROVIDER, nothing more. The
// draw is its controller support — the community gamepad mapping database,
// hotplug, rumble, battery, and DualSense gyro/touchpad — which is the one
// part of raw input not worth hand-writing three times. Gameplay still binds
// to actions through InputManager and never sees SDL (input directives), and
// the latency-critical mouse/keyboard path stays with the native hid backends
// unless measurement says otherwise.
//
// KNOWN LIMIT (deliberate, being ported next): SDL's event queue is
// main-thread-bound — a Cocoa requirement, not a preference — so an SDL3 hid
// backend cannot park a thread in the OS wait primitive the way IOHIDManager
// does. That is fine for gamepads (250 Hz–1 kHz, ns-stamped events) and is the
// open question for mouse.
class Sdl3Platform final : public IPlatform {
public:
    bool init(const PlatformConfig& cfg) override;
    void shutdown() override;

    void* nativeWindowHandle() const override;
    void* nativeDisplayHandle() const override;
    void* backendWindowHandle() const override;

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
    void  setNativeEventHook(NativeEventHook hook) override;

    // UI input (ui_input.h)
    void  enableUiInput(bool on) override { m_uiInput = on; }
    void  takeUiEvents(std::vector<uiin::Event>& out) override;
    void  windowSize(int& w, int& h) const override;
    std::string clipboardText() const override;
    void  setClipboardText(const std::string& utf8) override;
    void  setUiCursor(uiin::Cursor cursor) override;
    void  setTextInput(bool active, float x, float y, float w, float h) override;

    // Tool windows (IToolWindow)
    std::unique_ptr<IToolWindow> createToolWindow(const PlatformConfig& cfg) override;
    void  contentOrigin(float& x, float& y) const override;
    bool  globalPointer(float& x, float& y, bool& primaryDown) const override;

private:
    friend class Sdl3ToolWindow;
    void translateUiEvent(const SDL_Event& e);
    std::unordered_map<uint32_t, Sdl3ToolWindow*> m_tools;   // by SDL_WindowID

    NativeEventHook m_eventHook;
    bool                     m_uiInput = false;
    std::vector<uiin::Event> m_uiEvents;
    bool                     m_textInputOn = false;
    uiin::Cursor             m_cursor = uiin::Cursor::Arrow;
    SDL_Cursor*              m_cursors[(int)uiin::Cursor::Hidden] = {};
    SDL_Window* m_window      = nullptr;
    bool        m_shouldClose = false;
    bool        m_ownsSdl     = false;   // did WE call SDL_Init?
};
