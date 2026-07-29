#include "runtime/platform/sdl3_platform.h"

#include <SDL3/SDL.h>

#include <cstdio>

bool Sdl3Platform::init(const PlatformConfig& cfg) {
    // Video only. Gamepads are initialized by the hid backend when it lands,
    // so a headless/dedicated build never spins up input subsystems it has no
    // use for. SDL_Init is refcounted, so both can coexist.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::printf("[Platform] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        m_ownsSdl = true;
    }

    // No SDL_WINDOW_OPENGL/VULKAN/METAL flag: bgfx creates its own device from
    // the native handle, and asking SDL for a graphics-API window here would
    // make it build a context bgfx then ignores.
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE
                                | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    m_window = SDL_CreateWindow(cfg.title.c_str(), cfg.width, cfg.height, flags);
    if (!m_window) {
        std::printf("[Platform] SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (m_ownsSdl) { SDL_Quit(); m_ownsSdl = false; }
        return false;
    }
    m_shouldClose = false;
    return true;
}

void Sdl3Platform::shutdown() {
    if (m_window) { SDL_DestroyWindow(m_window); m_window = nullptr; }
    if (m_ownsSdl) { SDL_Quit(); m_ownsSdl = false; }
}

void* Sdl3Platform::nativeWindowHandle() const {
    if (!m_window) return nullptr;
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(SDL_PLATFORM_MACOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_WIN32)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    // Wayland reports a surface pointer; X11 reports a window ID as a number.
    if (void* surf = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))
        return surf;
    return (void*)(uintptr_t)SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
#else
    (void)props;
    return nullptr;
#endif
}

void* Sdl3Platform::nativeDisplayHandle() const {
    if (!m_window) return nullptr;
#if defined(SDL_PLATFORM_LINUX)
    const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    if (void* wl = SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr))
        return wl;
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
#else
    return nullptr;   // macOS/Windows: the window handle is sufficient
#endif
}

void* Sdl3Platform::backendWindowHandle() const { return m_window; }

void Sdl3Platform::setNativeEventHook(NativeEventHook hook) {
    m_eventHook = std::move(hook);
}

void Sdl3Platform::pollEvents() {
    // SDL has ONE process-wide event queue, unlike GLFW's per-window
    // callbacks: whoever pumps it sees every window's events. Drain it fully
    // here and fan every event out to the hook, since ImGui and the window
    // input source both need to see them but only one component can own the
    // pump.
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        // Observers first (ImGui, the window input source) — they must see
        // every event, including the close request we act on below.
        if (m_eventHook) m_eventHook(&e);
        switch (e.type) {
            case SDL_EVENT_QUIT:
                m_shouldClose = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (m_window &&
                    e.window.windowID == SDL_GetWindowID(m_window))
                    m_shouldClose = true;
                break;
            default: break;
        }
    }
}

bool Sdl3Platform::shouldClose() const { return m_shouldClose; }
void Sdl3Platform::requestClose()      { m_shouldClose = true; }

void Sdl3Platform::framebufferSize(int& w, int& h) const {
    if (!m_window) { w = 0; h = 0; return; }
    // In PIXELS, not logical points — the distinction matters on retina and
    // on fractional-scaling Wayland, and bgfx wants pixels.
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
}

void Sdl3Platform::waitEvents(double timeoutSeconds) {
    SDL_Event e;
    const Sint32 ms = timeoutSeconds <= 0.0
                    ? 0 : (Sint32)(timeoutSeconds * 1000.0);
    // Peek with a timeout, then drain through the normal path so close
    // handling lives in exactly one place.
    if (SDL_WaitEventTimeout(&e, ms)) {
        SDL_PushEvent(&e);
        pollEvents();
    }
}

void Sdl3Platform::setTitle(const std::string& title) {
    if (m_window) SDL_SetWindowTitle(m_window, title.c_str());
}

void Sdl3Platform::setCursorMode(CursorMode mode) {
    if (!m_window) return;
    // Relative mode hides the cursor, confines it, and switches SDL to raw
    // relative deltas — the same bundle GLFW's CURSOR_DISABLED gives.
    SDL_SetWindowRelativeMouseMode(m_window, mode == CursorMode::Captured);
}
