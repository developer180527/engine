// ── window_ops_glfw — GLFW implementation of the per-window seam ─────────────
// Compiled into engine_runtime alongside glfw_platform.cpp; the two are the
// same backend choice made twice, one for the single window a game has and one
// for the many windows a tool has. See window_ops.h for why the seam exists and
// why it is not editor-only.
#include "runtime/platform/window_ops.h"

#if defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND) || __has_include(<wayland-client.h>)
        #define GLFW_EXPOSE_NATIVE_WAYLAND
    #endif
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// The engine-owned Key/MouseButton constants (input_event.h, backend-free) are
// numerically identical to GLFW's, so this backend casts directly rather than
// translating — the SDL3 implementation of the same header needs a lookup table
// instead, which is exactly the kind of difference this seam exists to hold.
//
// These assertions used to live in input_system.h, which meant that header had
// to include <GLFW/glfw3.h> to state them. They belong HERE: they are a claim
// about the GLFW backend, and asserting them in the one TU that relies on the
// equivalence is both more accurate and free of consequence for anyone else.
static_assert((int)Key::Space      == GLFW_KEY_SPACE);
static_assert((int)Key::Escape     == GLFW_KEY_ESCAPE);
static_assert((int)Key::Enter      == GLFW_KEY_ENTER);
static_assert((int)Key::W          == GLFW_KEY_W);
static_assert((int)Key::Num9       == GLFW_KEY_9);
static_assert((int)Key::F12        == GLFW_KEY_F12);
static_assert((int)Key::LeftShift  == GLFW_KEY_LEFT_SHIFT);
static_assert((int)Key::RightSuper == GLFW_KEY_RIGHT_SUPER);
static_assert((int)MouseButton::Left   == GLFW_MOUSE_BUTTON_LEFT);
static_assert((int)MouseButton::Middle == GLFW_MOUSE_BUTTON_MIDDLE);
static_assert(kKeyCodeMax     == GLFW_KEY_LAST);
static_assert(kMouseButtonMax == GLFW_MOUSE_BUTTON_LAST);

namespace {
inline GLFWwindow* win(wsi::WindowHandle w) {
    return static_cast<GLFWwindow*>(w);
}

// One sink, because there is one window input source. Per-window sinks would
// be a different feature and nothing asks for it.
wsi::InputSink g_sink{};
GLFWscrollfun  g_prevScroll = nullptr;
GLFWcharfun    g_prevChar   = nullptr;

void cbScroll(GLFWwindow* w, double x, double y) {
    if (g_sink.onScroll) g_sink.onScroll(g_sink.ctx, x, y);
    if (g_prevScroll) g_prevScroll(w, x, y);   // chain to ImGui
}
void cbChar(GLFWwindow* w, unsigned int cp) {
    if (g_sink.onText) g_sink.onText(g_sink.ctx, cp);
    if (g_prevChar) g_prevChar(w, cp);         // chain to ImGui
}
} // namespace

namespace wsi {

bool isFocused(WindowHandle w) {
    return w && glfwGetWindowAttrib(win(w), GLFW_FOCUSED);
}

bool isKeyDown(WindowHandle w, Key k) {
    if (!w || k == Key::Unknown) return false;
    return glfwGetKey(win(w), static_cast<int>(k)) == GLFW_PRESS;
}

bool isMouseButtonDown(WindowHandle w, MouseButton b) {
    if (!w) return false;
    return glfwGetMouseButton(win(w), static_cast<int>(b)) == GLFW_PRESS;
}

void cursorPos(WindowHandle w, double& x, double& y) {
    if (!w) { x = 0.0; y = 0.0; return; }
    glfwGetCursorPos(win(w), &x, &y);
}

CursorMode cursorMode(WindowHandle w) {
    if (!w) return CursorMode::Normal;
    return glfwGetInputMode(win(w), GLFW_CURSOR) == GLFW_CURSOR_DISABLED
         ? CursorMode::Captured : CursorMode::Normal;
}

void setCursorMode(WindowHandle w, CursorMode mode) {
    if (!w) return;
    glfwSetInputMode(win(w), GLFW_CURSOR,
        mode == CursorMode::Captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void framebufferSize(WindowHandle w, int& fbW, int& fbH) {
    if (!w) { fbW = 0; fbH = 0; return; }
    glfwGetFramebufferSize(win(w), &fbW, &fbH);
}

void requestClose(WindowHandle w) {
    if (w) glfwSetWindowShouldClose(win(w), GLFW_TRUE);
}

void* nativeWindowHandle(WindowHandle w) {
    if (!w) return nullptr;
#if defined(__APPLE__)
    return (void*)glfwGetCocoaWindow(win(w));
#elif defined(_WIN32)
    return (void*)glfwGetWin32Window(win(w));
#elif defined(__linux__)
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandWindow(win(w));
    #endif
    return (void*)(uintptr_t)glfwGetX11Window(win(w));
#else
    return nullptr;
#endif
}

const char* keyName(Key k) {
    if (k == Key::Unknown) return nullptr;
    return glfwGetKeyName(static_cast<int>(k), 0);
}

void pollKeyboard(WindowHandle w, bool* out, int count) {
    if (!out || count <= 0) return;
    for (int i = 0; i < count; ++i) out[i] = false;
    if (!w) return;
    auto* gw = win(w);
    // From 32 (Space): GLFW's 0..31 are UNKNOWN and assorted non-keys, and
    // querying them is meaningless rather than merely wasteful.
    const int last = count - 1 < kKeyCodeMax ? count - 1 : kKeyCodeMax;
    for (int k = 32; k <= last; ++k)
        out[k] = (glfwGetKey(gw, k) == GLFW_PRESS);
}

void pollMouseButtons(WindowHandle w, bool* out, int count) {
    if (!out || count <= 0) return;
    for (int i = 0; i < count; ++i) out[i] = false;
    if (!w) return;
    auto* gw = win(w);
    const int last = count - 1 < kMouseButtonMax ? count - 1 : kMouseButtonMax;
    for (int b = 0; b <= last; ++b)
        out[b] = (glfwGetMouseButton(gw, b) == GLFW_PRESS);
}

void installInputSink(WindowHandle w, InputSink sink) {
    g_sink = sink;
    if (!w) return;
    // Captured so they can be chained: ImGui installs its own and both need to
    // run. Installing before the UI backend would make ImGui overwrite these
    // instead, which is why the contract says to call this afterwards.
    //
    // Keep the ORIGINAL previous callback if we are already installed. GLFW
    // hands back whatever was registered, so a second install would store our
    // own function as the one to chain to — and the next scroll event would
    // recurse until the stack ran out. Not reachable today (the editor installs
    // once), but the failure mode is a stack overflow far from its cause, and
    // this is the cheapest possible place to make it impossible.
    GLFWscrollfun prevS = glfwSetScrollCallback(win(w), cbScroll);
    GLFWcharfun   prevC = glfwSetCharCallback  (win(w), cbChar);
    if (prevS != cbScroll) g_prevScroll = prevS;
    if (prevC != cbChar)   g_prevChar   = prevC;
}

void feedNativeEvent(const void* /*nativeEvent*/) {
    // GLFW has no event struct to forward; scroll and text arrive through the
    // callbacks installed above.
}

} // namespace wsi
