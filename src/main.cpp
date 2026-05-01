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
#include "components/spinner.h"
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
    bx::Vec3 position { 0.0f, 6.0f, 18.0f };  // pulled back + slightly elevated to see the 3x3 grid  // start a bit back from origin
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

    // Register the cube as a Mesh asset. The Mesh takes ownership of the
    // bgfx buffer handles via move semantics; when assets goes out of scope,
    // its Mesh destructor calls bgfx::destroy(). We keep local vbh/ibh as
    // observers (bgfx handles are just IDs) so the existing render code
    // still works; Step 5 will switch the render loop to look them up
    // through the handle.
    const uint32_t cubeIndexCount = sizeof(kCubeIndices) / sizeof(kCubeIndices[0]);
    MeshHandle     cubeMesh       = assets.addMesh(Mesh{ vbh, ibh, cubeIndexCount });
    (void)cubeMesh;  // unused this step; Step 5 will consume it

    bgfx::RendererType::Enum rendererType = bgfx::getRendererType();
    bgfx::ProgramHandle program = bgfx::createProgram(
        bgfx::createEmbeddedShader(kShaders, rendererType, "vs_triangle"),
        bgfx::createEmbeddedShader(kShaders, rendererType, "fs_triangle"),
        true);

    imguiInit(window, 16.0f);

    // ---- Spawn a single entity to verify flecs ----
    // Components are registered implicitly the first time they're used.
    // The entity gets a Transform (default: identity), a MeshRenderer
    // pointing at our cube mesh, and a Name for the editor's hierarchy panel.
    // Spawn a 3x3 grid of cubes, all sharing the same mesh asset.
    // Each gets a unique name (for the hierarchy panel) and a slightly
    // different spinner speed so they rotate out of sync visually.
    constexpr int   kGridSize    = 3;
    constexpr float kGridSpacing = 5.0f;  // world units between cubes
    constexpr float kGridOffset  = -(kGridSize - 1) * 0.5f * kGridSpacing;

    for (int row = 0; row < kGridSize; ++row) {
        for (int col = 0; col < kGridSize; ++col) {
            char nameBuf[32];
            std::snprintf(nameBuf, sizeof(nameBuf), "Cube (%d,%d)", row, col);

            Transform t;
            t.position = {
                kGridOffset + col * kGridSpacing,
                0.0f,
                kGridOffset + row * kGridSpacing
            };

            // Vary spinner speeds so the grid looks lively, not mechanical.
            const float yawSpeed   = 0.4f + 0.05f * (row * kGridSize + col);
            const float pitchSpeed = 0.3f + 0.04f * (col * kGridSize + row);

            ecs.entity(nameBuf)
                .set<Transform>(t)
                .set<MeshRenderer>({ cubeMesh })
                .set<Name>({ nameBuf })
                .set<Spinner>({ yawSpeed, pitchSpeed });
        }
    }

    // Sanity check: count entities with Transform + MeshRenderer.
    // Should be 1. If flecs reports a different number, something's wrong
    // with component registration.
    int renderableCount = 0;
    ecs.query_builder<Transform, MeshRenderer>()
        .build()
        .each([&](flecs::entity, Transform&, MeshRenderer&) {
            ++renderableCount;
        });
    std::printf("[ECS] Renderable entity count: %d\n", renderableCount);

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

        // ---- Spinner system: update rotations over time ----
        // Reads Spinner, writes Transform. First example of an ECS system in
        // the engine: a query that operates on every entity matching the
        // component pattern, no matter how many.
        ecs.query_builder<Transform, const Spinner>()
            .build()
            .each([dt](flecs::entity, Transform& t, const Spinner& s) {
                // Build incremental rotation quats for this frame, then compose
                // them with the entity's current rotation. bx::fromAxisAngle
                // returns a Quaternion by value (its default ctor is deleted).
                const bx::Quaternion qYaw   = bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
                const bx::Quaternion qPitch = bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);

                // Compose: new = pitchDelta * yawDelta * current
                t.rotation = bx::mul(qPitch, bx::mul(qYaw, t.rotation));
                // Re-normalize each frame to prevent drift from accumulated FP error.
                t.rotation = bx::normalize(t.rotation);
            });

        // ---- Render system: submit a draw call per renderable entity ----
        // Iterates every entity with Transform + MeshRenderer. Adding more
        // such entities (Step 6) will draw more cubes automatically.
        ecs.query_builder<const Transform, const MeshRenderer>()
            .build()
            .each([&](flecs::entity, const Transform& t, const MeshRenderer& mr) {
                const Mesh* mesh = assets.getMesh(mr.mesh);
                if (!mesh) return;  // missing-asset placeholder will go here later

                float model[16];
                t.getMatrix(model);
                bgfx::setTransform(model);

                bgfx::setVertexBuffer(0, mesh->vbh);
                bgfx::setIndexBuffer(mesh->ibh);
                bgfx::setState(BGFX_STATE_DEFAULT);
                bgfx::submit(kSceneView, program);
            });

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
    // Note: vbh/ibh are no longer destroyed here — the AssetRegistry owns
    // them now via the Mesh asset. Mesh's destructor will call bgfx::destroy
    // when assets goes out of scope at the end of main().
    imguiShutdown();

    // Release all GPU mesh resources while bgfx is still alive.
    // Without this, the registry's destructor would run after main returns,
    // by which time bgfx::shutdown() has already torn down the renderer —
    // bgfx::destroy() calls on dead handles are technically undefined behavior.
    assets.clear();

    bgfx::shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
