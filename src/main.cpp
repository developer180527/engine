#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <GLFW/glfw3.h>
#if defined(__APPLE__)
    #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#include <imgui.h>
#include "render/imgui_bgfx.h"

#include <cstdio>
#include <cstdint>

static void* getNativeWindowHandle(GLFWwindow* w) {
#if defined(__APPLE__)
    return glfwGetCocoaWindow(w);
#elif defined(_WIN32)
    return glfwGetWin32Window(w);
#elif defined(__linux__)
    return (void*)(uintptr_t)glfwGetX11Window(w);
#else
    return nullptr;
#endif
}

static void* getNativeDisplayHandle() {
#if defined(__linux__)
    return glfwGetX11Display();
#else
    return nullptr;
#endif
}

int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Engine [milestone 1b]", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "glfwCreateWindow failed\n"); glfwTerminate(); return 1; }

    // Enforce a minimum window size so the user can't drag below something sane.
    glfwSetWindowSizeLimits(window, 640, 360, GLFW_DONT_CARE, GLFW_DONT_CARE);

    bgfx::renderFrame(); // single-threaded mode (macOS requirement, harmless elsewhere)

    bgfx::PlatformData pd{};
    pd.nwh = getNativeWindowHandle(window);
    pd.ndt = getNativeDisplayHandle();

    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    bgfx::Init init;
    init.type = bgfx::RendererType::Count;
    init.resolution.width  = (uint32_t)fbw;
    init.resolution.height = (uint32_t)fbh;
    init.resolution.reset  = BGFX_RESET_VSYNC;
    init.platformData = pd;

    if (!bgfx::init(init)) { std::fprintf(stderr, "bgfx::init failed\n"); glfwDestroyWindow(window); glfwTerminate(); return 1; }

    constexpr bgfx::ViewId kClearView = 0;
    bgfx::setViewClear(kClearView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);

    imguiInit(window, 16.0f);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Skip rendering when minimized or zero-sized; bgfx and ImGui both dislike 0×N frames.
        int newW, newH;
        glfwGetFramebufferSize(window, &newW, &newH);
        if (newW <= 0 || newH <= 0) {
            glfwWaitEventsTimeout(0.1); // yield instead of busy-spinning
            continue;
        }

        if (newW != fbw || newH != fbh) {
            fbw = newW; fbh = newH;
            bgfx::reset((uint32_t)fbw, (uint32_t)fbh, BGFX_RESET_VSYNC);
            bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);
        }

        imguiNewFrame();
        ImGui::ShowDemoWindow();
        imguiRender(255);

        bgfx::touch(kClearView);
        bgfx::frame();
    }

    imguiShutdown();
    bgfx::shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
