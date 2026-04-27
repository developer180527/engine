#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <cstdio>

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Engine [milestone 1a]", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    // On macOS, bgfx must run single-threaded because Cocoa requires the main thread.
    // Calling renderFrame() once before init() puts bgfx in single-threaded mode.
    bgfx::renderFrame();

    bgfx::PlatformData pd{};
    pd.nwh = glfwGetCocoaWindow(window);
    pd.ndt = nullptr;

    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.resolution.width  = static_cast<uint32_t>(fbw);
    init.resolution.height = static_cast<uint32_t>(fbh);
    init.resolution.reset  = BGFX_RESET_VSYNC;
    init.platformData = pd;

    if (!bgfx::init(init)) {
        std::fprintf(stderr, "bgfx::init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    constexpr bgfx::ViewId kClearView = 0;
    bgfx::setViewClear(kClearView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int newW, newH;
        glfwGetFramebufferSize(window, &newW, &newH);
        if (newW != fbw || newH != fbh) {
            fbw = newW; fbh = newH;
            bgfx::reset(static_cast<uint32_t>(fbw), static_cast<uint32_t>(fbh), BGFX_RESET_VSYNC);
            bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);
        }

        bgfx::touch(kClearView);
        bgfx::frame();
    }

    bgfx::shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
