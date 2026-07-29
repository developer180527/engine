#pragma once
#include "runtime/platform/platform.h"

struct SDL_Window;

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
    void  setNativeEventHook(NativeEventHook hook) override;

private:
    NativeEventHook m_eventHook;
    SDL_Window* m_window      = nullptr;
    bool        m_shouldClose = false;
    bool        m_ownsSdl     = false;   // did WE call SDL_Init?
};
