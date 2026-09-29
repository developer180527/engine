#include "runtime/platform/hdr_surface.h"
#include "runtime/platform/glfw_platform.h"

#include <cstdio>

// Platform-specific native window handle for bgfx
#if defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    // Wayland first, X11 fallback — matches modern distro defaults.
    // GLFW 3.4+ exposes the Wayland surface via glfwGetWaylandWindow();
    // older GLFW only has X11. Both defines are harmless if the backend
    // isn't present: we pick at runtime below.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND) || __has_include(<wayland-client.h>)
        #define GLFW_EXPOSE_NATIVE_WAYLAND
    #endif
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "runtime/platform/title_bar.h"

bool GlfwPlatform::init(const PlatformConfig& cfg) {
    if (!glfwInit()) {
        std::printf("[Platform] GLFW init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(cfg.width, cfg.height,
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::printf("[Platform] Window creation failed\n");
        return false;
    }
    // AFTER creation, not a window hint: the point is to keep the window fully
    // decorated and only stop the title bar being drawn. A GLFW_DECORATED hint
    // would do the opposite and cost resize/snap/minimise/maximise.
    if (cfg.hideTitleBar && !platwin::hideTitleBar(nativeWindowHandle()))
        std::printf("[Platform] title bar hiding not implemented on this "
                    "platform - using the system title bar\n");
    return true;
}

void uiForgetWindow(GLFWwindow* w);   // below, with the UI input callbacks

void GlfwPlatform::shutdown() {
    if (m_window) uiForgetWindow(m_window);
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}

void* GlfwPlatform::nativeWindowHandle() const {
#if defined(__APPLE__)
    return glfwGetCocoaWindow(m_window);
#elif defined(_WIN32)
    return glfwGetWin32Window(m_window);
#elif defined(__linux__)
    // Prefer Wayland when available (GLFW 3.4+); fall back to X11.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandWindow(m_window);
    #endif
    return (void*)glfwGetX11Window(m_window);
#else
    #error "Unsupported platform — add native window handle retrieval"
#endif
}

void* GlfwPlatform::nativeDisplayHandle() const {
#if defined(__linux__)
    // bgfx needs the server connection alongside the window on Linux
    // (PlatformData::ndt); on macOS/Windows there is nothing to pass.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandDisplay();
    #endif
    return (void*)glfwGetX11Display();
#else
    return nullptr;
#endif
}

void GlfwPlatform::pollEvents() {
    glfwPollEvents();
}

bool GlfwPlatform::shouldClose() const {
    return m_window && glfwWindowShouldClose(m_window);
}

void GlfwPlatform::requestClose() {
    if (m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void GlfwPlatform::framebufferSize(int& w, int& h) const {
    if (m_window) glfwGetFramebufferSize(m_window, &w, &h);
    else { w = 0; h = 0; }
}

void GlfwPlatform::waitEvents(double timeoutSeconds) {
    glfwWaitEventsTimeout(timeoutSeconds);
}

void GlfwPlatform::setTitle(const std::string& title) {
    if (m_window) glfwSetWindowTitle(m_window, title.c_str());
}

void GlfwPlatform::setCursorMode(CursorMode mode) {
    if (!m_window) return;
    glfwSetInputMode(m_window, GLFW_CURSOR,
        mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

// ── HDR output (colour stage C) ─────────────────────────────────────────────
bool GlfwPlatform::enableHdrOutput() {
    return platwin::enableExtendedDynamicRange(nativeWindowHandle());
}
float GlfwPlatform::hdrHeadroom() const {
    return platwin::extendedDynamicRangeHeadroom(nativeWindowHandle());
}
std::string GlfwPlatform::hdrSurfaceDescription() const {
    return platwin::describeHdrSurface(nativeWindowHandle());
}

// ── UI input (ui_input.h) ───────────────────────────────────────────────────
// GLFW dispatches through per-window callbacks, so enabling UI input installs
// a set that appends to the window's SINK and then CALLS WHATEVER WAS THERE
// BEFORE — ImGui's backend and window_ops' input sink keep working. Windows
// are found through a small registry rather than the window user pointer,
// which belongs to the application. Tool windows use the same callbacks with
// their own sink.
#include <cmath>
#include <unordered_map>

namespace {

using Sink = GlfwPlatform::UiSink;

struct Prev {
    GLFWcursorposfun    pos    = nullptr;
    GLFWmousebuttonfun  button = nullptr;
    GLFWscrollfun       scroll = nullptr;
    GLFWkeyfun          key    = nullptr;
    GLFWcharfun         chr    = nullptr;
    GLFWcursorenterfun  enter  = nullptr;
    GLFWwindowfocusfun  focus  = nullptr;
    GLFWwindowclosefun  close  = nullptr;
};
std::unordered_map<GLFWwindow*, std::pair<Sink*, Prev>> g_ui;

Sink* sinkOf(GLFWwindow* w, Prev*& prev) {
    auto it = g_ui.find(w);
    if (it == g_ui.end()) { prev = nullptr; return nullptr; }
    prev = &it->second.second;
    return it->second.first;
}

uiin::Modifiers modsFrom(int m) {
    uiin::Modifiers r;
    r.shift = (m & GLFW_MOD_SHIFT) != 0;
    r.ctrl  = (m & GLFW_MOD_CONTROL) != 0;
    r.alt   = (m & GLFW_MOD_ALT) != 0;
    r.super = (m & GLFW_MOD_SUPER) != 0;
    return r;
}

void push(Sink* s, uiin::Event&& e) {
    if (s && s->enabled) s->events.push_back(std::move(e));
}

void cbPos(GLFWwindow* w, double x, double y) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    uiin::Event e; e.kind = uiin::EventKind::PointerMoved; e.x = (float)x; e.y = (float)y;
    push(s, std::move(e));
    if (prev && prev->pos) prev->pos(w, x, y);
}
void cbButton(GLFWwindow* w, int button, int action, int mods) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    uiin::Event e; e.kind = uiin::EventKind::PointerButton;
    bool known = true;
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:   e.button = uiin::PointerButton::Primary;   break;
    case GLFW_MOUSE_BUTTON_RIGHT:  e.button = uiin::PointerButton::Secondary; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: e.button = uiin::PointerButton::Middle;    break;
    case GLFW_MOUSE_BUTTON_4:      e.button = uiin::PointerButton::Back;      break;
    case GLFW_MOUSE_BUTTON_5:      e.button = uiin::PointerButton::Forward;   break;
    default: known = false;
    }
    if (known && s) {
        double x = 0, y = 0; glfwGetCursorPos(w, &x, &y);
        e.pressed = action == GLFW_PRESS; e.x = (float)x; e.y = (float)y;
        e.mods = s->mods = modsFrom(mods);
        push(s, std::move(e));
    }
    if (prev && prev->button) prev->button(w, button, action, mods);
}
void cbScroll(GLFWwindow* w, double dx, double dy) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    // GLFW reports lines (fractional for trackpads) and not which device.
    uiin::Event e; e.kind = uiin::EventKind::Wheel; e.unit = uiin::WheelUnit::Line;
    e.x = (float)dx; e.y = (float)dy;
    push(s, std::move(e));
    if (prev && prev->scroll) prev->scroll(w, dx, dy);
}
void cbKey(GLFWwindow* w, int key, int sc, int action, int mods) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    // Key values ARE GLFW's (input_event.h), so this is a range check, not a map.
    if (s && key >= 32 && key <= kKeyCodeMax) {
        uiin::Event e; e.kind = uiin::EventKind::Key; e.key = (Key)key;
        e.pressed = action != GLFW_RELEASE; e.repeat = action == GLFW_REPEAT;
        e.mods = s->mods = modsFrom(mods);
        push(s, std::move(e));
    }
    if (prev && prev->key) prev->key(w, key, sc, action, mods);
}
void cbChar(GLFWwindow* w, unsigned int cp) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    char t[5] = {};
    if (cp < 0x80)          { t[0] = (char)cp; }
    else if (cp < 0x800)    { t[0] = (char)(0xC0 | (cp >> 6));  t[1] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)  { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              t[2] = (char)(0x80 | (cp & 0x3F)); }
    else                    { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                              t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); }
    uiin::Event e; e.kind = uiin::EventKind::Text; e.text = t;
    push(s, std::move(e));
    if (prev && prev->chr) prev->chr(w, cp);
}
void cbEnter(GLFWwindow* w, int entered) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    if (!entered) { uiin::Event e; e.kind = uiin::EventKind::PointerLeft; push(s, std::move(e)); }
    if (prev && prev->enter) prev->enter(w, entered);
}
void cbFocus(GLFWwindow* w, int focused) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    if (!focused) { uiin::Event e; e.kind = uiin::EventKind::FocusLost; push(s, std::move(e)); }
    if (prev && prev->focus) prev->focus(w, focused);
}
void cbClose(GLFWwindow* w) {
    Prev* prev; Sink* s = sinkOf(w, prev);
    if (s) s->close = true;
    if (prev && prev->close) prev->close(w);
}

// Install once per window. A second install would record OUR callbacks as
// "previous" and recurse forever (window_ops_glfw.cpp has the same guard).
void install(GLFWwindow* w, Sink* sink, bool watchClose) {
    Prev prev;
    prev.pos    = glfwSetCursorPosCallback  (w, cbPos);
    prev.button = glfwSetMouseButtonCallback(w, cbButton);
    prev.scroll = glfwSetScrollCallback     (w, cbScroll);
    prev.key    = glfwSetKeyCallback        (w, cbKey);
    prev.chr    = glfwSetCharCallback       (w, cbChar);
    prev.enter  = glfwSetCursorEnterCallback(w, cbEnter);
    prev.focus  = glfwSetWindowFocusCallback(w, cbFocus);
    if (watchClose) prev.close = glfwSetWindowCloseCallback(w, cbClose);
    g_ui[w] = { sink, prev };
}

GLFWcursor* g_cursors[(int)uiin::Cursor::Hidden] = {};

void applyCursor(GLFWwindow* w, uiin::Cursor cursor) {
    if (cursor == uiin::Cursor::Hidden) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        return;
    }
    glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    int shape = GLFW_ARROW_CURSOR;
    switch (cursor) {
    case uiin::Cursor::Text:       shape = GLFW_IBEAM_CURSOR;          break;
    case uiin::Cursor::Hand:       shape = GLFW_POINTING_HAND_CURSOR;  break;
    case uiin::Cursor::Crosshair:  shape = GLFW_CROSSHAIR_CURSOR;      break;
    case uiin::Cursor::Move:       shape = GLFW_RESIZE_ALL_CURSOR;     break;
    case uiin::Cursor::NotAllowed: shape = GLFW_NOT_ALLOWED_CURSOR;    break;
    case uiin::Cursor::ResizeEW:   shape = GLFW_RESIZE_EW_CURSOR;      break;
    case uiin::Cursor::ResizeNS:   shape = GLFW_RESIZE_NS_CURSOR;      break;
    case uiin::Cursor::ResizeNWSE: shape = GLFW_RESIZE_NWSE_CURSOR;    break;
    case uiin::Cursor::ResizeNESW: shape = GLFW_RESIZE_NESW_CURSOR;    break;
    default: break;   // Wait has no GLFW shape: arrow
    }
    GLFWcursor*& c = g_cursors[(int)cursor];
    if (!c) c = glfwCreateStandardCursor(shape);
    glfwSetCursor(w, c);   // null restores the default arrow
}

class GlfwToolWindow final : public IToolWindow {
public:
    explicit GlfwToolWindow(GLFWwindow* w) : m_window(w) {
        m_sink.enabled = true;
        install(w, &m_sink, /*watchClose*/ true);
    }
    ~GlfwToolWindow() override { g_ui.erase(m_window); glfwDestroyWindow(m_window); }
    void* nativeWindowHandle() const override {
#if defined(__APPLE__)
        return glfwGetCocoaWindow(m_window);
#elif defined(_WIN32)
        return glfwGetWin32Window(m_window);
#else
        return (void*)glfwGetX11Window(m_window);
#endif
    }
    void windowSize(int& w, int& h) const override { glfwGetWindowSize(m_window, &w, &h); }
    void framebufferSize(int& w, int& h) const override { glfwGetFramebufferSize(m_window, &w, &h); }
    // glfwGetWindowPos is the client area's position.
    void contentOrigin(float& x, float& y) const override {
        int ix = 0, iy = 0; glfwGetWindowPos(m_window, &ix, &iy); x = (float)ix; y = (float)iy;
    }
    void setContentOrigin(float x, float y) override {
        glfwSetWindowPos(m_window, (int)std::lround(x), (int)std::lround(y));
    }
    void setVisible(bool v) override { if (v) glfwShowWindow(m_window); else glfwHideWindow(m_window); }
    bool closeRequested() const override { return m_sink.close; }
    void takeUiEvents(std::vector<uiin::Event>& out) override {
        for (auto& e : m_sink.events) out.push_back(std::move(e));
        m_sink.events.clear();
    }
    void setUiCursor(uiin::Cursor c) override {
        if (c != m_cursor) { m_cursor = c; applyCursor(m_window, c); }
    }

private:
    GLFWwindow*  m_window;
    Sink         m_sink;
    uiin::Cursor m_cursor = uiin::Cursor::Arrow;
};

}  // namespace

void uiForgetWindow(GLFWwindow* w) { g_ui.erase(w); }

void GlfwPlatform::enableUiInput(bool on) {
    m_ui.enabled = on;
    if (!on || m_uiInstalled || !m_window) return;
    install(m_window, &m_ui, /*watchClose*/ false);   // the main window's close is glfwWindowShouldClose
    m_uiInstalled = true;
}

void GlfwPlatform::takeUiEvents(std::vector<uiin::Event>& out) {
    for (auto& e : m_ui.events) out.push_back(std::move(e));
    m_ui.events.clear();
}

void GlfwPlatform::windowSize(int& w, int& h) const {
    if (m_window) glfwGetWindowSize(m_window, &w, &h);
    else { w = 0; h = 0; }
}

std::string GlfwPlatform::clipboardText() const {
    const char* t = m_window ? glfwGetClipboardString(m_window) : nullptr;
    return t ? t : "";
}

void GlfwPlatform::setClipboardText(const std::string& utf8) {
    if (m_window) glfwSetClipboardString(m_window, utf8.c_str());
}

void GlfwPlatform::setUiCursor(uiin::Cursor cursor) {
    if (!m_window || cursor == m_cursor) return;
    m_cursor = cursor;
    applyCursor(m_window, cursor);
}

std::unique_ptr<IToolWindow> GlfwPlatform::createToolWindow(const PlatformConfig& cfg) {
    // Hidden until placed; no client API — bgfx drives it.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(cfg.width, cfg.height, cfg.title.c_str(), nullptr, nullptr);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (!w) { std::printf("[Platform] tool window: creation failed\n"); return nullptr; }
    return std::make_unique<GlfwToolWindow>(w);
}

void GlfwPlatform::contentOrigin(float& x, float& y) const {
    int ix = 0, iy = 0;
    if (m_window) glfwGetWindowPos(m_window, &ix, &iy);
    x = (float)ix; y = (float)iy;
}

bool GlfwPlatform::globalPointer(float& x, float& y, bool& primaryDown) const {
    // GLFW has no desktop pointer query; the main window's cursor position is
    // reported relative to it even outside it, so add its origin.
    if (!m_window) return false;
    double cx = 0, cy = 0; glfwGetCursorPos(m_window, &cx, &cy);
    int wx = 0, wy = 0;    glfwGetWindowPos(m_window, &wx, &wy);
    x = (float)(wx + cx); y = (float)(wy + cy);
    primaryDown = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    return true;
}
