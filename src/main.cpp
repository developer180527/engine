#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bgfx/embedded_shader.h>
#include <bx/math.h>

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

// Shader bytecode for every backend bgfx might pick.
// Each header defines a static array (e.g. vs_triangle_mtl, vs_triangle_glsl, ...)
// BGFX_EMBEDDED_SHADER below references all of these by name.
#include "glsl/vs_triangle.sc.bin.h"
#include "glsl/fs_triangle.sc.bin.h"
#include "essl/vs_triangle.sc.bin.h"
#include "essl/fs_triangle.sc.bin.h"
#include "spirv/vs_triangle.sc.bin.h"
#include "spirv/fs_triangle.sc.bin.h"
#if defined(__APPLE__)
    #include "metal/vs_triangle.sc.bin.h"
    #include "metal/fs_triangle.sc.bin.h"
#endif

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

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

static const PosColorVertex kTriangleVertices[] = {
    { -0.5f, -0.5f, 0.0f, 0xff0000ff },  // bottom-left,  red
    {  0.5f, -0.5f, 0.0f, 0xff00ff00 },  // bottom-right, green
    {  0.0f,  0.5f, 0.0f, 0xffff0000 },  // top-center,   blue
};
static const uint16_t kTriangleIndices[] = { 0, 1, 2 };

// BGFX_EMBEDDED_SHADER references vs_triangle_glsl, vs_triangle_essl,
// vs_triangle_spv, vs_triangle_mtl etc. — those names come from the
// included headers above. bgfx::createEmbeddedShader picks the right one
// at runtime based on the active renderer.
static const bgfx::EmbeddedShader kShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_triangle),
    BGFX_EMBEDDED_SHADER(fs_triangle),
    BGFX_EMBEDDED_SHADER_END(),
};

int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Engine [milestone 2]", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(window, 640, 360, GLFW_DONT_CARE, GLFW_DONT_CARE);

    bgfx::renderFrame();

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
    if (!bgfx::init(init)) { glfwDestroyWindow(window); glfwTerminate(); return 1; }

    constexpr bgfx::ViewId kSceneView = 0;
    bgfx::setViewClear(kSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0, bgfx::BackbufferRatio::Equal);

    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
        .end();

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::makeRef(kTriangleVertices, sizeof(kTriangleVertices)),
        layout
    );
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
        bgfx::makeRef(kTriangleIndices, sizeof(kTriangleIndices))
    );

    bgfx::RendererType::Enum rendererType = bgfx::getRendererType();
    bgfx::ProgramHandle program = bgfx::createProgram(
        bgfx::createEmbeddedShader(kShaders, rendererType, "vs_triangle"),
        bgfx::createEmbeddedShader(kShaders, rendererType, "fs_triangle"),
        true
    );

    imguiInit(window, 16.0f);

    double lastTime = glfwGetTime();
    float  rotation = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int newW, newH;
        glfwGetFramebufferSize(window, &newW, &newH);
        if (newW <= 0 || newH <= 0) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }
        if (newW != fbw || newH != fbh) {
            fbw = newW; fbh = newH;
            bgfx::reset((uint32_t)fbw, (uint32_t)fbh, BGFX_RESET_VSYNC);
            bgfx::setViewRect(kSceneView, 0, 0, bgfx::BackbufferRatio::Equal);
        }

        double now = glfwGetTime();
        float  dt  = float(now - lastTime);
        lastTime = now;
        rotation += dt;

        const bx::Vec3 eye = { 0.0f, 0.0f, -3.0f };
        const bx::Vec3 at  = { 0.0f, 0.0f,  0.0f };
        const bx::Vec3 up  = { 0.0f, 1.0f,  0.0f };

        float view[16];
        bx::mtxLookAt(view, eye, at, up);

        float proj[16];
        bx::mtxProj(
            proj, 60.0f, float(fbw) / float(fbh), 0.1f, 100.0f,
            bgfx::getCaps()->homogeneousDepth
        );

        bgfx::setViewTransform(kSceneView, view, proj);

        float model[16];
        bx::mtxRotateY(model, rotation);
        bgfx::setTransform(model);

        bgfx::setVertexBuffer(0, vbh);
        bgfx::setIndexBuffer(ibh);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                       | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA);
        bgfx::submit(kSceneView, program);

        imguiNewFrame();
        ImGui::Begin("Stats");
        ImGui::Text("FPS: %.1f", 1.0f / dt);
        ImGui::Text("Frame: %.2f ms", dt * 1000.0f);
        ImGui::Text("Renderer: %s", bgfx::getRendererName(rendererType));
        ImGui::End();
        imguiRender(255);

        bgfx::touch(kSceneView);
        bgfx::frame();
    }

    bgfx::destroy(program);
    bgfx::destroy(ibh);
    bgfx::destroy(vbh);
    imguiShutdown();
    bgfx::shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
