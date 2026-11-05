#include "runtime/platform/hdr_surface.h"
#include "runtime/platform/glfw_platform.h"

#include <algorithm>
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

void installCloseCallback(GlfwPlatform* p);   // with the window control below

bool GlfwPlatform::init(const PlatformConfig& cfg) {
    if (!glfwInit()) {
        std::printf("[Platform] GLFW init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, cfg.startHidden ? GLFW_FALSE : GLFW_TRUE);
    m_window = glfwCreateWindow(cfg.width, cfg.height,
                                cfg.title.c_str(), nullptr, nullptr);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);   // hints persist; leave the default
    if (!m_window) {
        std::printf("[Platform] Window creation failed\n");
        return false;
    }
    installCloseCallback(this);
    // AFTER creation, not a window hint: the point is to keep the window fully
    // decorated and only stop the title bar being drawn. A GLFW_DECORATED hint
    // would do the opposite and cost resize/snap/minimise/maximise.
    if (cfg.hideTitleBar && !platwin::hideTitleBar(nativeWindowHandle()))
        std::printf("[Platform] title bar hiding not implemented on this "
                    "platform - using the system title bar\n");
    return true;
}

void uiForgetWindow(GLFWwindow* w);   // below, with the UI input callbacks
void forgetCloseCallback(GLFWwindow* w);

void GlfwPlatform::shutdown() {
    if (m_window) uiForgetWindow(m_window);
    if (m_window) forgetCloseCallback(m_window);
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
    #error "port: the GLFW main window needs native window handle retrieval"
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
#else  // any OS: only X11/Wayland pass a display connection alongside the window
    return nullptr;
#endif
}

void GlfwPlatform::pollEvents() {
    glfwPollEvents();
    gatherWindowEvents();
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
#elif defined(__linux__)
        // Same order as GlfwPlatform::nativeWindowHandle. This used to be the
        // #else, so a tool window on a Wayland session handed out an X11 id
        // while the main window handed out a Wayland surface.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
        if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
            return (void*)glfwGetWaylandWindow(m_window);
    #endif
        return (void*)glfwGetX11Window(m_window);
#else
#  error "port: GLFW tool windows need native window handle retrieval"
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

// ── Main-window control (WO-070, the C API's eng_window.h) ──────────────────
// Window events are found by comparing the window's state after each pump
// with the state before it, not by callbacks: GLFW allows one callback per
// kind per window, and ImGui and the input system already own several. The
// close request is the exception (it must be intercepted to be declined), so
// its callback chains to whatever was installed before it.
namespace {
std::unordered_map<GLFWwindow*, GlfwPlatform*>& closeOwners() {
    static std::unordered_map<GLFWwindow*, GlfwPlatform*> m;
    return m;
}
void onClose(GLFWwindow* w) {
    auto it = closeOwners().find(w);
    if (it != closeOwners().end()) {
        GlfwPlatform* p = it->second;
        WindowEvent e; e.type = WindowEvent::Type::CloseRequested;
        p->m_windowEvents.push_back(e);
        // A game that handles close decides; anything else closes as before.
        if (p->m_handleClose) glfwSetWindowShouldClose(w, GLFW_FALSE);
        if (p->m_prevCloseCallback) p->m_prevCloseCallback(w);
    }
}
bool isWayland() {
#if defined(__linux__) && defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    return glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else  // any OS: only Linux has Wayland; everywhere else a window may be placed and re-moded
    return false;
#endif
}
DisplayModeDesc toDesc(const GLFWvidmode* m) {
    DisplayModeDesc d;
    if (!m) return d;
    d.width = m->width; d.height = m->height; d.refreshMilliHz = m->refreshRate * 1000;
    return d;
}
} // namespace

void installCloseCallback(GlfwPlatform* p) {
    closeOwners()[p->glfwWindow()] = p;
    p->m_prevCloseCallback = glfwSetWindowCloseCallback(p->glfwWindow(), onClose);
}
void forgetCloseCallback(GLFWwindow* w) { closeOwners().erase(w); }

uint32_t GlfwPlatform::idOf(GLFWmonitor* m) {
    if (!m) return 0;
    for (size_t i = 0; i < m_monitorIds.size(); ++i)
        if (m_monitorIds[i] == m) return (uint32_t)(i + 1);
    m_monitorIds.push_back(m);
    return (uint32_t)m_monitorIds.size();
}

GLFWmonitor* GlfwPlatform::monitorById(uint32_t id) {
    if (id == 0 || id > m_monitorIds.size()) return nullptr;
    GLFWmonitor* m = m_monitorIds[id - 1];
    // Still connected? A disconnected monitor's pointer is dangling.
    int n = 0;
    GLFWmonitor** all = glfwGetMonitors(&n);
    for (int i = 0; i < n; ++i) if (all[i] == m) return m;
    return nullptr;
}

bool GlfwPlatform::canExclusiveFullscreen() const {
    return glfwInit() && !isWayland();   // Wayland gives no mode changes
}
bool GlfwPlatform::canPositionWindow() const {
    return glfwInit() && !isWayland();   // Wayland lets no client place itself
}

std::vector<MonitorDesc> GlfwPlatform::monitors() {
    std::vector<MonitorDesc> out;
    if (!glfwInit()) return out;   // idempotent; listing may precede the window
    int n = 0;
    GLFWmonitor** all = glfwGetMonitors(&n);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    for (int i = 0; i < n; ++i) {
        MonitorDesc d;
        d.id = idOf(all[i]);
        const char* name = glfwGetMonitorName(all[i]);
        d.name = name ? name : "";
        d.primary = all[i] == primary;
        glfwGetMonitorPos(all[i], &d.x, &d.y);
        float sx = 1.0f, sy = 1.0f;
        glfwGetMonitorContentScale(all[i], &sx, &sy);
        d.contentScale = sx;
        d.currentMode = toDesc(glfwGetVideoMode(all[i]));
        d.width = d.currentMode.width; d.height = d.currentMode.height;
        out.push_back(d);
    }
    return out;
}

std::vector<DisplayModeDesc> GlfwPlatform::monitorModes(uint32_t monitorId) {
    std::vector<DisplayModeDesc> out;
    if (!glfwInit()) return out;
    GLFWmonitor* m = monitorById(monitorId);
    if (!m) return out;
    int n = 0;
    const GLFWvidmode* modes = glfwGetVideoModes(m, &n);
    for (int i = 0; i < n; ++i) out.push_back(toDesc(&modes[i]));
    return out;
}

bool GlfwPlatform::windowState(WindowState& s) const {
    if (!m_window) return false;
    s.mode = glfwGetWindowMonitor(m_window) ? m_mode : WindowMode::Windowed;
    glfwGetWindowPos(m_window, &s.x, &s.y);
    glfwGetWindowSize(m_window, &s.width, &s.height);
    glfwGetFramebufferSize(m_window, &s.widthPixels, &s.heightPixels);
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(m_window, &sx, &sy);
    s.contentScale = sx;
    s.focused   = glfwGetWindowAttrib(m_window, GLFW_FOCUSED) != 0;
    s.minimised = glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0;
    s.visible   = glfwGetWindowAttrib(m_window, GLFW_VISIBLE) != 0;
    // The monitor: the fullscreen one, else the one holding the window's centre.
    s.monitor = 0;
    auto* self = const_cast<GlfwPlatform*>(this);
    if (GLFWmonitor* fm = glfwGetWindowMonitor(m_window)) {
        s.monitor = self->idOf(fm);
    } else {
        const int cx = s.x + s.width / 2, cy = s.y + s.height / 2;
        int n = 0;
        GLFWmonitor** all = glfwGetMonitors(&n);
        for (int i = 0; i < n; ++i) {
            int mx = 0, my = 0;
            glfwGetMonitorPos(all[i], &mx, &my);
            const GLFWvidmode* vm = glfwGetVideoMode(all[i]);
            if (vm && cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) {
                s.monitor = self->idOf(all[i]);
                break;
            }
        }
        if (!s.monitor) s.monitor = self->idOf(glfwGetPrimaryMonitor());
    }
    return true;
}

bool GlfwPlatform::setWindowMode(WindowMode mode, uint32_t monitorId,
                                 const DisplayModeDesc* exclusive) {
    if (!m_window) return false;
    if (mode == WindowMode::Windowed) {
        if (glfwGetWindowMonitor(m_window)) {
            const int w = m_windowedW > 0 ? m_windowedW : 1280;
            const int h = m_windowedH > 0 ? m_windowedH : 720;
            glfwSetWindowMonitor(m_window, nullptr, m_windowedX, m_windowedY, w, h,
                                 GLFW_DONT_CARE);
        }
        m_mode = mode;
        return true;
    }
    if (mode == WindowMode::ExclusiveFullscreen && !canExclusiveFullscreen()) return false;
    GLFWmonitor* mon = monitorId ? monitorById(monitorId) : nullptr;
    if (monitorId && !mon) return false;          // asked for one that is gone
    if (!mon) {
        WindowState s;
        windowState(s);
        mon = monitorById(s.monitor);
        if (!mon) mon = glfwGetPrimaryMonitor();
    }
    if (!mon) return false;
    if (!glfwGetWindowMonitor(m_window)) {         // remember where to go back to
        glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
        glfwGetWindowSize(m_window, &m_windowedW, &m_windowedH);
    }
    const GLFWvidmode* cur = glfwGetVideoMode(mon);
    if (!cur) return false;
    int w = cur->width, h = cur->height, hz = cur->refreshRate;
    // Borderless keeps the desktop's mode: covering the monitor changes nothing
    // the OS has to re-mode. Exclusive takes the requested mode (the OS picks the
    // closest; the MODE_CHANGED event and windowState report what it chose).
    if (mode == WindowMode::ExclusiveFullscreen && exclusive && exclusive->width > 0) {
        w = exclusive->width; h = exclusive->height;
        hz = exclusive->refreshMilliHz > 0 ? (exclusive->refreshMilliHz + 500) / 1000 : GLFW_DONT_CARE;
    }
    glfwSetWindowMonitor(m_window, mon, 0, 0, w, h, hz);
    m_mode = mode;
    return true;
}

bool GlfwPlatform::setWindowSize(int w, int h) {
    if (!m_window || w <= 0 || h <= 0) return false;
    // GLFW's limits bind the USER's resizing; a size set from code goes
    // straight through (measured on macOS: 2000x2000 past a 1000x800 maximum).
    // The limits are the window's, whoever asks, so code is clamped too.
    if (m_minW) w = std::max(w, m_minW);
    if (m_minH) h = std::max(h, m_minH);
    if (m_maxW) w = std::min(w, m_maxW);
    if (m_maxH) h = std::min(h, m_maxH);
    glfwSetWindowSize(m_window, w, h);
    return true;
}

bool GlfwPlatform::setWindowPosition(int x, int y) {
    if (!m_window || isWayland()) return false;   // Wayland lets no client place itself
    glfwSetWindowPos(m_window, x, y);
    return true;
}

bool GlfwPlatform::setWindowSizeLimits(int minW, int minH, int maxW, int maxH) {
    if (!m_window) return false;
    auto v = [](int n) { return n > 0 ? n : GLFW_DONT_CARE; };
    glfwSetWindowSizeLimits(m_window, v(minW), v(minH), v(maxW), v(maxH));
    m_minW = std::max(minW, 0); m_minH = std::max(minH, 0);
    m_maxW = std::max(maxW, 0); m_maxH = std::max(maxH, 0);
    return true;
}

bool GlfwPlatform::setWindowAspect(int numerator, int denominator) {
    if (!m_window) return false;
    if (numerator > 0 && denominator > 0) glfwSetWindowAspectRatio(m_window, numerator, denominator);
    else glfwSetWindowAspectRatio(m_window, GLFW_DONT_CARE, GLFW_DONT_CARE);
    return true;
}

bool GlfwPlatform::setWindowFlags(uint32_t flags) {
    if (!m_window) return false;
    glfwSetWindowAttrib(m_window, GLFW_RESIZABLE, (flags & kWindowFixedSize) ? GLFW_FALSE : GLFW_TRUE);
    glfwSetWindowAttrib(m_window, GLFW_DECORATED, (flags & kWindowNoDecoration) ? GLFW_FALSE : GLFW_TRUE);
    glfwSetWindowAttrib(m_window, GLFW_FLOATING, (flags & kWindowAlwaysOnTop) ? GLFW_TRUE : GLFW_FALSE);
    m_handleClose = (flags & kWindowHandleClose) != 0;
    return platwin::setUserFullscreenEnabled(nativeWindowHandle(),
                                             (flags & kWindowNoUserModeChange) == 0);
}

bool GlfwPlatform::showWindow(bool visible) {
    if (!m_window) return false;
    if (visible) glfwShowWindow(m_window); else glfwHideWindow(m_window);
    return true;
}

bool GlfwPlatform::focusWindow() {
    if (!m_window) return false;
    glfwFocusWindow(m_window);
    return true;
}

void GlfwPlatform::gatherWindowEvents() {
    if (!m_window) return;
    WindowState s;
    if (!windowState(s)) return;
    if (m_haveLastState) {
        const WindowState& o = m_lastState;
        using T = WindowEvent::Type;
        auto push = [&](T t) { WindowEvent e; e.type = t; m_windowEvents.push_back(e); return &m_windowEvents.back(); };
        if (s.width != o.width || s.height != o.height ||
            s.widthPixels != o.widthPixels || s.heightPixels != o.heightPixels) {
            auto* e = push(T::Resized);
            e->width = s.width; e->height = s.height; e->a = s.widthPixels; e->b = s.heightPixels;
        }
        if (s.x != o.x || s.y != o.y) { auto* e = push(T::Moved); e->x = s.x; e->y = s.y; }
        if (s.focused != o.focused) push(T::FocusChanged)->a = s.focused ? 1 : 0;
        if (s.minimised != o.minimised) push(T::MinimisedChanged)->a = s.minimised ? 1 : 0;
        if (s.mode != o.mode) push(T::ModeChanged)->a = (int)s.mode;
        if (s.monitor != o.monitor) push(T::MonitorChanged)->monitor = s.monitor;
    }
    m_lastState = s;
    m_haveLastState = true;
    if (!m_closedReported && glfwWindowShouldClose(m_window)) {
        m_closedReported = true;
        WindowEvent e; e.type = WindowEvent::Type::Closed;
        m_windowEvents.push_back(e);
    }
}

void GlfwPlatform::takeWindowEvents(std::vector<WindowEvent>& out) {
    out.insert(out.end(), m_windowEvents.begin(), m_windowEvents.end());
    m_windowEvents.clear();
}
