#include "runtime/platform/hdr_surface.h"
#include "runtime/platform/sdl3_platform.h"
#include "runtime/platform/title_bar.h"
#include "runtime/input/sdl3_keymap.h"

#include <SDL3/SDL.h>

#include <cmath>
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
    // Same seam as the GLFW backend, and for the same reason: the window stays
    // decorated (no SDL_WINDOW_BORDERLESS), only the bar's drawing is
    // suppressed. See runtime/platform/title_bar.h.
    if (cfg.hideTitleBar && !platwin::hideTitleBar(nativeWindowHandle()))
        std::printf("[Platform] title bar hiding not implemented on this "
                    "platform - using the system title bar\n");
    m_shouldClose = false;
    return true;
}

void Sdl3Platform::shutdown() {
    setLiveResizeHook({});
    for (SDL_Cursor*& c : m_cursors)
        if (c) { SDL_DestroyCursor(c); c = nullptr; }
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
#  error "port: the SDL3 main window needs native window handle retrieval"
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
#else  // any OS: only X11/Wayland pass a display connection alongside the window
    return nullptr;
#endif
}

void* Sdl3Platform::backendWindowHandle() const { return m_window; }

void Sdl3Platform::setNativeEventHook(NativeEventHook hook) {
    m_eventHook = std::move(hook);
}

// Live resize: on macOS a window-edge drag runs the OS's modal loop inside
// SDL_PumpEvents, so pollEvents does not return until the mouse is released.
// SDL still delivers SDL_EVENT_WINDOW_EXPOSED (data1 = 1) to event WATCHERS
// during that loop, on the main thread, and expects a redraw from there. A
// queued event would only arrive after the drag, which is the stretching.
void Sdl3Platform::setLiveResizeHook(LiveResizeHook hook) {
    const bool had = static_cast<bool>(m_liveResizeHook);
    m_liveResizeHook = std::move(hook);
    if (m_liveResizeHook && !had)      SDL_AddEventWatch(&Sdl3Platform::liveResizeWatch, this);
    else if (!m_liveResizeHook && had) SDL_RemoveEventWatch(&Sdl3Platform::liveResizeWatch, this);
}

bool Sdl3Platform::liveResizeWatch(void* self, SDL_Event* e) {
    auto* p = static_cast<Sdl3Platform*>(self);
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1 &&
        p->m_liveResizeHook && !p->m_inLiveResizeHook) {
        p->m_inLiveResizeHook = true;   // the frame it draws must not re-enter
        p->m_liveResizeHook();
        p->m_inLiveResizeHook = false;
    }
    return true;   // a watcher's return value is ignored
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
        translateUiEvent(e);   // no-op unless UI input is on or a tool window exists
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

// ── HDR output (colour stage C) ─────────────────────────────────────────────
bool Sdl3Platform::enableHdrOutput() {
    return platwin::enableExtendedDynamicRange(nativeWindowHandle());
}
float Sdl3Platform::hdrHeadroom() const {
    return platwin::extendedDynamicRangeHeadroom(nativeWindowHandle());
}
std::string Sdl3Platform::hdrSurfaceDescription() const {
    return platwin::describeHdrSurface(nativeWindowHandle());
}

// ── UI input (ui_input.h) ───────────────────────────────────────────────────
namespace {

uiin::Modifiers modsFrom(SDL_Keymod m) {
    uiin::Modifiers r;
    r.shift = (m & SDL_KMOD_SHIFT) != 0;
    r.ctrl  = (m & SDL_KMOD_CTRL)  != 0;
    r.alt   = (m & SDL_KMOD_ALT)   != 0;
    r.super = (m & SDL_KMOD_GUI)   != 0;
    return r;
}

// SDL reports the IME cursor in CHARACTERS; UI text offsets are UTF-8 bytes.
uint32_t utf8ByteOffset(const char* s, int chars) {
    if (!s || chars <= 0) return 0;
    uint32_t i = 0;
    for (int c = 0; s[i] && c < chars; ++c) {
        ++i;
        while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
    }
    return i;
}

}  // namespace

// ── Tool windows ────────────────────────────────────────────────────────────
class Sdl3ToolWindow final : public IToolWindow {
public:
    Sdl3ToolWindow(Sdl3Platform* owner, SDL_Window* w) : m_owner(owner), m_window(w) {
        m_owner->m_tools[SDL_GetWindowID(w)] = this;
    }
    ~Sdl3ToolWindow() override {
        m_owner->m_tools.erase(SDL_GetWindowID(m_window));
        if (m_textInputOn) SDL_StopTextInput(m_window);
        SDL_DestroyWindow(m_window);
    }
    void* nativeWindowHandle() const override {
        const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(SDL_PLATFORM_MACOS)
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_WIN32)
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
        if (void* surf = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))
            return surf;
        return (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
#else
#  error "port: SDL3 tool windows need native window handle retrieval"
#endif
    }
    void* nativeDisplayHandle() const override { return m_owner->nativeDisplayHandle(); }
    void windowSize(int& w, int& h) const override { SDL_GetWindowSize(m_window, &w, &h); }
    void framebufferSize(int& w, int& h) const override { SDL_GetWindowSizeInPixels(m_window, &w, &h); }
    // SDL3 reports the CLIENT area's position.
    void contentOrigin(float& x, float& y) const override {
        int ix = 0, iy = 0; SDL_GetWindowPosition(m_window, &ix, &iy); x = (float)ix; y = (float)iy;
    }
    void setContentOrigin(float x, float y) override {
        SDL_SetWindowPosition(m_window, (int)std::lround(x), (int)std::lround(y));
    }
    void setVisible(bool v) override { if (v) SDL_ShowWindow(m_window); else SDL_HideWindow(m_window); }
    bool closeRequested() const override { return m_close; }
    void takeUiEvents(std::vector<uiin::Event>& out) override {
        for (auto& e : m_events) out.push_back(std::move(e));
        m_events.clear();
    }
    void setUiCursor(uiin::Cursor c) override { m_owner->setUiCursor(c); }   // the cursor is global in SDL
    void setTextInput(bool active, float x, float y, float w, float h) override {
        if (active) {
            const SDL_Rect r{ (int)x, (int)y, (int)w, (int)h };
            SDL_SetTextInputArea(m_window, &r, 0);
            if (!m_textInputOn) SDL_StartTextInput(m_window);
        } else if (m_textInputOn) {
            SDL_StopTextInput(m_window);
        }
        m_textInputOn = active;
    }

    std::vector<uiin::Event> m_events;
    bool                     m_close = false;

private:
    Sdl3Platform* m_owner;
    SDL_Window*   m_window;
    bool          m_textInputOn = false;
};

std::unique_ptr<IToolWindow> Sdl3Platform::createToolWindow(const PlatformConfig& cfg) {
    // Hidden until placed: a window that appears and then jumps is the thing
    // every host gets wrong first.
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
                                | SDL_WINDOW_HIDDEN;
    SDL_Window* w = SDL_CreateWindow(cfg.title.c_str(), cfg.width, cfg.height, flags);
    if (!w) {
        std::printf("[Platform] tool window: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return nullptr;
    }
    return std::make_unique<Sdl3ToolWindow>(this, w);
}

void Sdl3Platform::contentOrigin(float& x, float& y) const {
    int ix = 0, iy = 0;
    if (m_window) SDL_GetWindowPosition(m_window, &ix, &iy);
    x = (float)ix; y = (float)iy;
}

bool Sdl3Platform::globalPointer(float& x, float& y, bool& primaryDown) const {
    const SDL_MouseButtonFlags b = SDL_GetGlobalMouseState(&x, &y);
    primaryDown = (b & SDL_BUTTON_LMASK) != 0;
    return true;
}

void Sdl3Platform::translateUiEvent(const SDL_Event& e) {
    // SDL's queue is process-wide: find which window this event is for, then
    // whose queue that is — the main window's, or a tool window's.
    SDL_WindowID wid = 0;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:       wid = e.motion.windowID; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:    wid = e.button.windowID; break;
    case SDL_EVENT_MOUSE_WHEEL:        wid = e.wheel.windowID;  break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:             wid = e.key.windowID;    break;
    case SDL_EVENT_TEXT_INPUT:         wid = e.text.windowID;   break;
    case SDL_EVENT_TEXT_EDITING:       wid = e.edit.windowID;   break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: wid = e.window.windowID; break;
    default: return;
    }
    std::vector<uiin::Event>* queue = nullptr;
    Sdl3ToolWindow* tool = nullptr;
    if (m_window && wid == SDL_GetWindowID(m_window)) {
        if (!m_uiInput) return;
        queue = &m_uiEvents;
    } else if (auto it = m_tools.find(wid); it != m_tools.end()) {
        tool = it->second;
        queue = &tool->m_events;
    } else {
        return;
    }

    uiin::Event ev;
    switch (e.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (tool) tool->m_close = true;   // the main window's is handled in pollEvents
        return;
    case SDL_EVENT_MOUSE_MOTION:
        ev.kind = uiin::EventKind::PointerMoved;
        ev.x = e.motion.x; ev.y = e.motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ev.kind = uiin::EventKind::PointerButton;
        switch (e.button.button) {
        case SDL_BUTTON_LEFT:   ev.button = uiin::PointerButton::Primary;   break;
        case SDL_BUTTON_RIGHT:  ev.button = uiin::PointerButton::Secondary; break;
        case SDL_BUTTON_MIDDLE: ev.button = uiin::PointerButton::Middle;    break;
        case SDL_BUTTON_X1:     ev.button = uiin::PointerButton::Back;      break;
        case SDL_BUTTON_X2:     ev.button = uiin::PointerButton::Forward;   break;
        default: return;
        }
        ev.pressed = e.button.down;
        ev.x = e.button.x; ev.y = e.button.y;
        ev.mods = modsFrom(SDL_GetModState());
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        ev.kind = uiin::EventKind::Wheel;
        // SDL3 does not say whether a trackpad or a wheel produced this. Its
        // values are in wheel "lines" (fractional for precise devices), so
        // they go out as lines, unconverted; FLIPPED is undone so positive y
        // always means "content moves down", as on every other backend.
        const float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        ev.x = e.wheel.x * flip; ev.y = e.wheel.y * flip;
        ev.unit = uiin::WheelUnit::Line;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        ev.kind = uiin::EventKind::PointerLeft;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ev.kind = uiin::EventKind::FocusLost;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        ev.kind    = uiin::EventKind::Key;
        ev.key     = sdl3keys::fromScancode(e.key.scancode);   // physical position
        ev.pressed = e.key.down;
        ev.repeat  = e.key.repeat;
        ev.mods    = modsFrom(e.key.mod);
        if (ev.key == Key::Unknown) return;
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!e.text.text) return;
        ev.kind = uiin::EventKind::Text;
        ev.text = e.text.text;
        break;
    case SDL_EVENT_TEXT_EDITING:
        ev.kind   = uiin::EventKind::ImePreedit;
        ev.text   = e.edit.text ? e.edit.text : "";
        ev.cursor = utf8ByteOffset(e.edit.text, e.edit.start);
        break;
    default:
        return;
    }
    queue->push_back(std::move(ev));
}

void Sdl3Platform::takeUiEvents(std::vector<uiin::Event>& out) {
    for (auto& e : m_uiEvents) out.push_back(std::move(e));
    m_uiEvents.clear();
}

void Sdl3Platform::windowSize(int& w, int& h) const {
    if (!m_window) { w = 0; h = 0; return; }
    SDL_GetWindowSize(m_window, &w, &h);   // logical points
}

std::string Sdl3Platform::clipboardText() const {
    char* t = SDL_GetClipboardText();       // never null; "" when empty
    std::string s = t ? t : "";
    SDL_free(t);
    return s;
}

void Sdl3Platform::setClipboardText(const std::string& utf8) {
    SDL_SetClipboardText(utf8.c_str());
}

void Sdl3Platform::setUiCursor(uiin::Cursor cursor) {
    if (cursor == m_cursor) return;
    m_cursor = cursor;
    if (cursor == uiin::Cursor::Hidden) { SDL_HideCursor(); return; }
    SDL_ShowCursor();
    SDL_SystemCursor sys = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (cursor) {
    case uiin::Cursor::Text:       sys = SDL_SYSTEM_CURSOR_TEXT;        break;
    case uiin::Cursor::Hand:       sys = SDL_SYSTEM_CURSOR_POINTER;     break;
    case uiin::Cursor::Crosshair:  sys = SDL_SYSTEM_CURSOR_CROSSHAIR;   break;
    case uiin::Cursor::Move:       sys = SDL_SYSTEM_CURSOR_MOVE;        break;
    case uiin::Cursor::NotAllowed: sys = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
    case uiin::Cursor::Wait:       sys = SDL_SYSTEM_CURSOR_WAIT;        break;
    case uiin::Cursor::ResizeEW:   sys = SDL_SYSTEM_CURSOR_EW_RESIZE;   break;
    case uiin::Cursor::ResizeNS:   sys = SDL_SYSTEM_CURSOR_NS_RESIZE;   break;
    case uiin::Cursor::ResizeNWSE: sys = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
    case uiin::Cursor::ResizeNESW: sys = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
    default: break;
    }
    SDL_Cursor*& c = m_cursors[(int)cursor];
    if (!c) c = SDL_CreateSystemCursor(sys);
    if (c) SDL_SetCursor(c);
}

void Sdl3Platform::setTextInput(bool active, float x, float y, float w, float h) {
    if (!m_window) return;
    if (active) {
        const SDL_Rect r{ (int)x, (int)y, (int)w, (int)h };
        SDL_SetTextInputArea(m_window, &r, 0);
        if (!m_textInputOn) SDL_StartTextInput(m_window);
    } else if (m_textInputOn) {
        SDL_StopTextInput(m_window);
    }
    m_textInputOn = active;
}
