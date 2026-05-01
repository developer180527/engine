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

#include "glsl/vs_triangle.sc.bin.h"
#include "glsl/fs_triangle.sc.bin.h"
#include "essl/vs_triangle.sc.bin.h"
#include "essl/fs_triangle.sc.bin.h"
#include "spirv/vs_triangle.sc.bin.h"
#include "spirv/fs_triangle.sc.bin.h"
#include "metal/vs_triangle.sc.bin.h"
#include "metal/fs_triangle.sc.bin.h"

#include <cstdio>
#include <cstdint>
#include <cmath>

// ECS + engine-internal headers
#include <flecs.h>
#include "core/handle.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "render/mesh.h"
#include "render/asset_registry.h"

// ---------------- Native handle helpers ----------------
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

// ---------------- Cube geometry ----------------
struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

// 8 corners of a unit cube centered at origin, each with a distinct color
// Color encoding is little-endian ABGR (bgfx convention).
static const PosColorVertex kCubeVertices[] = {
    { -1.0f,  1.0f,  1.0f, 0xff000000 },
    {  1.0f,  1.0f,  1.0f, 0xff0000ff },
    { -1.0f, -1.0f,  1.0f, 0xff00ff00 },
    {  1.0f, -1.0f,  1.0f, 0xff00ffff },
    { -1.0f,  1.0f, -1.0f, 0xffff0000 },
    {  1.0f,  1.0f, -1.0f, 0xffff00ff },
    { -1.0f, -1.0f, -1.0f, 0xffffff00 },
    {  1.0f, -1.0f, -1.0f, 0xffffffff },
};

// 36 indices = 12 triangles = 6 faces.
// Winding is CCW when viewed from outside (front-facing in bgfx default).
static const uint16_t kCubeIndices[] = {
    0, 1, 2,  1, 3, 2,   // front  (+Z)
    4, 6, 5,  5, 6, 7,   // back   (-Z)
    0, 2, 4,  4, 2, 6,   // left   (-X)
    1, 5, 3,  5, 7, 3,   // right  (+X)
    0, 4, 1,  4, 5, 1,   // top    (+Y)
    2, 3, 6,  6, 3, 7,   // bottom (-Y)
};

static const bgfx::EmbeddedShader kShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_triangle),
    BGFX_EMBEDDED_SHADER(fs_triangle),
    BGFX_EMBEDDED_SHADER_END(),
};

// ---------------- Camera ----------------
// Free-fly camera. yaw/pitch in radians.
// yaw=0, pitch=0 means looking down -Z (matching default GL/bgfx convention).
struct Camera {
    bx::Vec3 position { 0.0f, 0.0f,  5.0f };  // start a bit back from origin
    float    yaw   = 0.0f;
    float    pitch = 0.0f;

    // Forward / right / up unit vectors derived from yaw + pitch.
    bx::Vec3 forward() const {
        return {
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
           -std::cos(yaw) * std::cos(pitch)
        };
    }
    bx::Vec3 right() const {
        return { std::cos(yaw), 0.0f, std::sin(yaw) };
    }
    bx::Vec3 up() const {
        return { 0.0f, 1.0f, 0.0f };  // world-up; common for FPS-style cameras
    }

    void getViewMatrix(float out[16]) const {
        const bx::Vec3 fwd = forward();
        const bx::Vec3 at  = bx::add(position, fwd);
        bx::mtxLookAt(out, position, at, up());
    }
};

// ---------------- Input state ----------------
// Tracks mouse-look state across frames.
struct InputState {
    bool   rightMouseHeld = false;
    double lastMouseX     = 0.0;
    double lastMouseY     = 0.0;
};

static void updateCamera(Camera& cam, InputState& input, GLFWwindow* window, float dt) {
    ImGuiIO& io = ImGui::GetIO();

    // Only suppress input if the user is actively typing in a text field.
    // (WantCaptureKeyboard is broader and includes ImGui nav focus, which we don't want
    // to block WASD on.)
    const bool typing = io.WantTextInput;

    // ---- Mouse look (right-click + drag) ----
    const bool rightDownNow = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
                              && !io.WantCaptureMouse;

    if (rightDownNow && !input.rightMouseHeld) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(window, &input.lastMouseX, &input.lastMouseY);
        input.rightMouseHeld = true;
    } else if (!rightDownNow && input.rightMouseHeld) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        input.rightMouseHeld = false;
    }

    if (input.rightMouseHeld) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        const float dx = float(mx - input.lastMouseX);
        const float dy = float(my - input.lastMouseY);
        input.lastMouseX = mx;
        input.lastMouseY = my;

        const float sensitivity = 0.0025f;
        cam.yaw   -= dx * sensitivity;
        cam.pitch -= dy * sensitivity;

        const float pitchLimit = bx::kPiHalf - 0.01f;
        if (cam.pitch >  pitchLimit) cam.pitch =  pitchLimit;
        if (cam.pitch < -pitchLimit) cam.pitch = -pitchLimit;
    }

    // ---- Keyboard movement (WASD/QE) ----
    if (typing) return;

    float speed = 5.0f;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
     || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
        speed *= 4.0f;
    }
    const float step = speed * dt;

    const bx::Vec3 fwd = cam.forward();
    const bx::Vec3 rt  = cam.right();
    const bx::Vec3 wup = { 0.0f, 1.0f, 0.0f };

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(fwd,  step));
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(fwd, -step));
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(rt,  -step));
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(rt,   step));
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(wup,  step));
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(wup, -step));
}

// ---------------- main ----------------
int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Engine [milestone 3]", nullptr, nullptr);
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

    // ---- ECS + asset registry ----
    // The flecs world owns all entities and their components for the engine's
    // lifetime. The AssetRegistry owns GPU mesh resources that components
    // reference by handle.
    flecs::world  ecs;
    AssetRegistry assets;

    constexpr bgfx::ViewId kSceneView = 0;
    bgfx::setViewClear(kSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0, bgfx::BackbufferRatio::Equal);

    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
        .end();

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::makeRef(kCubeVertices, sizeof(kCubeVertices)), layout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
        bgfx::makeRef(kCubeIndices, sizeof(kCubeIndices)));

    bgfx::RendererType::Enum rendererType = bgfx::getRendererType();
    bgfx::ProgramHandle program = bgfx::createProgram(
        bgfx::createEmbeddedShader(kShaders, rendererType, "vs_triangle"),
        bgfx::createEmbeddedShader(kShaders, rendererType, "fs_triangle"),
        true);

    imguiInit(window, 16.0f);

    Camera     camera;
    InputState input;

    double lastTime = glfwGetTime();
    float  spin     = 0.0f;

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

        const double now = glfwGetTime();
        const float  dt  = float(now - lastTime);
        lastTime = now;
        spin += dt * 0.5f;

        // ImGui must process input first so it can claim mouse/keyboard
        imguiNewFrame();

        updateCamera(camera, input, window, dt);

        // ---- View + projection ----
        float view[16];
        camera.getViewMatrix(view);

        float proj[16];
        bx::mtxProj(proj, 60.0f, float(fbw) / float(fbh), 0.1f, 1000.0f,
                    bgfx::getCaps()->homogeneousDepth);

        bgfx::setViewTransform(kSceneView, view, proj);

        // ---- Submit cube ----
        float model[16];
        bx::mtxRotateXY(model, spin * 0.7f, spin);
        bgfx::setTransform(model);

        bgfx::setVertexBuffer(0, vbh);
        bgfx::setIndexBuffer(ibh);
        bgfx::setState(BGFX_STATE_DEFAULT);  // includes RGBA write, depth test, MSAA, CCW front, back-cull
        bgfx::submit(kSceneView, program);

        // ---- Stats overlay ----
        ImGui::Begin("Stats");
        ImGui::Text("FPS: %.1f", 1.0f / dt);
        ImGui::Text("Frame: %.2f ms", dt * 1000.0f);
        ImGui::Text("Renderer: %s", bgfx::getRendererName(rendererType));
        ImGui::Separator();
        ImGui::Text("Camera pos: %.2f, %.2f, %.2f",
                    camera.position.x, camera.position.y, camera.position.z);
        ImGui::Text("Yaw: %.2f  Pitch: %.2f", camera.yaw, camera.pitch);
        ImGui::Separator();
        ImGui::TextWrapped("Hold right mouse + WASD/QE to fly. Shift = faster.");
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
