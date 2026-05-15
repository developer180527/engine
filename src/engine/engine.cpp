#include "engine/engine.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <imgui.h>
#include <ImGuizmo.h>
#include "render/imgui_bgfx.h"
#include "render/imgui_impl_glfw.h"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include "render/vertex.h"
#include "render/primitive_cube.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"
#include "io/gltf_importer.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "core/transform.h"

// Embedded shader headers generated at build time into build/shaders/.
// The IDE may show a red underline here — this is expected since the files
// don't exist until the first cmake+ninja run. The build itself works correctly
// because build/shaders is in the CMake include_directories.
// Shader headers are generated into build/shaders/<platform>/
// On macOS we target Metal. Add "spirv" or "glsl" variants for other platforms.
#include "metal/vs_triangle.sc.bin.h"
#include "metal/fs_triangle.sc.bin.h"

// ---- Constants ----
static constexpr bgfx::ViewId kSceneView = 0;
static constexpr int kGridSize    = 3;
static constexpr float kGridSpacing = 5.0f;

// ---- Engine ----

Engine::Engine()  = default;
Engine::~Engine() = default;

void Engine::setEditorLayer(std::unique_ptr<IEditorLayer> layer) {
    m_editorLayer = std::move(layer);
}

bool Engine::init(const EngineConfig& cfg) {
    m_width  = cfg.width;
    m_height = cfg.height;
    m_fov    = cfg.fov;

    if (!initPlatform(cfg))  return false;
    if (!initRenderer(cfg))  return false;
    if (!initSystems())      return false;

    buildDefaultScene();

    // Count what got created
    int entityCount = 0;
    m_ecs.query_builder<const Name>().build()
         .each([&](flecs::entity, const Name&) { ++entityCount; });
    std::printf("[Engine] Scene entities after init: %d\n", entityCount);

    if (m_editorLayer) m_editorLayer->init(*m_ctx);

    return true;
}

bool Engine::initPlatform(const EngineConfig& cfg) {
    if (!glfwInit()) {
        std::printf("[Engine] GLFW init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(cfg.width, cfg.height,
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::printf("[Engine] Window creation failed\n");
        return false;
    }
    glfwSetWindowUserPointer(m_window, this);
    return true;
}

bool Engine::initRenderer(const EngineConfig& cfg) {
    // bgfx single-threaded mode: renderFrame() BEFORE init() tells bgfx that
    // the render thread == main thread (required on macOS Metal).
    bgfx::renderFrame();

    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;

    // bgfx 1.14x: set the window handle through Init::platformData,
    // NOT via the deprecated bgfx::setPlatformData() free function.
    init.platformData.nwh  = glfwGetCocoaWindow(m_window);
    init.resolution.width  = (uint32_t)cfg.width;
    init.resolution.height = (uint32_t)cfg.height;
    init.resolution.reset  = BGFX_RESET_VSYNC;

    std::printf("[Engine] nwh = %p\n", init.platformData.nwh);
    if (!bgfx::init(init)) {
        std::printf("[Engine] bgfx init failed\n");
        return false;
    }

    bgfx::setViewClear(kSceneView,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1a1a1aff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0,
        (uint16_t)cfg.width, (uint16_t)cfg.height);

    // Build shader program
    m_program = bgfx::createProgram(
        bgfx::createShader(bgfx::makeRef(
            vs_triangle_mtl, sizeof(vs_triangle_mtl))),
        bgfx::createShader(bgfx::makeRef(
            fs_triangle_mtl, sizeof(fs_triangle_mtl))),
        true);

    if (!bgfx::isValid(m_program)) {
        std::printf("[Engine] Shader program creation failed\n");
        return false;
    }

    // Material uniforms
    m_sBaseColor   = bgfx::createUniform("s_baseColor",   bgfx::UniformType::Sampler);
    m_uParams      = bgfx::createUniform("u_params",      bgfx::UniformType::Vec4);
    m_uColorFactor = bgfx::createUniform("u_colorFactor", bgfx::UniformType::Vec4);

    // 1x1 white fallback texture
    static const uint32_t kWhite = 0xFFFFFFFFu;
    m_whiteTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0,
        bgfx::makeRef(&kWhite, 4));

    // ImGui — must be after bgfx is initialised, before first frame
    imguiInit(m_window, 16.0f);

    return true;
}

bool Engine::initSystems() {
    m_project = ProjectContext::autoDetect();
    std::printf("[Engine] Assets root: %s\n",
                m_project.assetsRoot.string().c_str());

    m_importers.registerImporter(std::make_unique<GltfImporter>());

    m_ctx = std::make_unique<EngineContext>(
        EngineContext{m_ecs, m_assets, m_textures,
                      m_materials, m_project, m_importers});

    return true;
}

void Engine::buildDefaultScene() {
    std::printf("[Engine] buildDefaultScene() called\n");
    // Cube grid — build mesh from raw primitive_cube namespace data
    bgfx::VertexBufferHandle cubeVbh = bgfx::createVertexBuffer(
        bgfx::makeRef(primitive_cube::kVertices,
                      sizeof(primitive_cube::kVertices)),
        Vertex::layout());
    bgfx::IndexBufferHandle cubeIbh = bgfx::createIndexBuffer(
        bgfx::makeRef(primitive_cube::kIndices,
                      sizeof(primitive_cube::kIndices)));
    Mesh cubeMesh(cubeVbh, cubeIbh,
        (uint32_t)(sizeof(primitive_cube::kIndices)
                   / sizeof(primitive_cube::kIndices[0])));
    MeshHandle cubeHandle = m_assets.addMesh(std::move(cubeMesh));

    for (int x = 0; x < kGridSize; ++x) {
        for (int z = 0; z < kGridSize; ++z) {
            char name[32];
            std::snprintf(name, sizeof(name), "Cube (%d,%d)", x, z);

            Transform t;
            t.position = {
                (x - kGridSize / 2) * kGridSpacing,
                0.0f,
                (z - kGridSize / 2) * kGridSpacing
            };

            m_ecs.entity(name)
                .set<Transform>(t)
                .set<MeshRenderer>({cubeHandle})
                .set<Name>({name})
                .set<Spinner>({0.3f, 0.1f});
        }
    }
}

void Engine::run() {
    using clock = std::chrono::steady_clock;
    auto prev   = clock::now();

    while (!glfwWindowShouldClose(m_window)) {
        auto  now = clock::now();
        float dt  = std::chrono::duration<float>(now - prev).count();
        prev = now;
        dt = std::min(dt, 0.05f);  // cap at 50ms

        glfwPollEvents();

        // ---- Editor hotkeys (before UI) ----
        if (m_editorLayer)
            m_editorLayer->handleHotkeys(m_window, *m_ctx);

        // ---- Input + camera ----
        processInput(dt);

        // ---- ECS systems ----
        tickSystems(dt);

        // ---- Render ----
        renderScene();

        // ---- Editor UI ----
        if (m_editorLayer) {
            m_editorLayer->render(*m_ctx);
        }

        bgfx::frame();
    }
}

void Engine::processInput(float dt) {
    // Right-mouse-button fly camera
    const bool rmbNow = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT)
                        == GLFW_PRESS;
    double mx, my;
    glfwGetCursorPos(m_window, &mx, &my);

    if (rmbNow) {
        if (m_rightMouseHeld) {
            float dx = (float)(mx - m_lastMouseX) * 0.003f;
            float dy = (float)(my - m_lastMouseY) * 0.003f;
            m_camera.yaw   += dx;
            m_camera.pitch = std::clamp(m_camera.pitch - dy,
                                        -bx::kPiHalf + 0.05f,
                                         bx::kPiHalf - 0.05f);
        }
        m_rightMouseHeld = true;

        const float speed = (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                            ? 20.0f : 7.0f;
        const float cp = std::cos(m_camera.pitch);
        const float sp = std::sin(m_camera.pitch);
        const float cy = std::cos(m_camera.yaw);
        const float sy = std::sin(m_camera.yaw);

        bx::Vec3 fwd {  sy * cp, -sp, cy * cp };
        bx::Vec3 right{ cy,       0,  -sy     };

        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS)
            m_camera.position = bx::mad(fwd,  {speed*dt,speed*dt,speed*dt}, m_camera.position);
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS)
            m_camera.position = bx::mad(fwd,  {-speed*dt,-speed*dt,-speed*dt}, m_camera.position);
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS)
            m_camera.position = bx::mad(right,{-speed*dt,-speed*dt,-speed*dt}, m_camera.position);
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS)
            m_camera.position = bx::mad(right,{speed*dt,speed*dt,speed*dt}, m_camera.position);
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS)
            m_camera.position.y -= speed * dt;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS)
            m_camera.position.y += speed * dt;
    } else {
        m_rightMouseHeld = false;
    }

    m_lastMouseX = mx;
    m_lastMouseY = my;
}

void Engine::tickSystems(float dt) {
    // Spinner system
    if (!ImGuizmo::IsUsing()) {
        m_ecs.query_builder<Transform, const Spinner>()
            .build()
            .each([dt](Transform& t, const Spinner& s) {
                bx::Quaternion qYaw   = bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
                bx::Quaternion qPitch = bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);
                t.rotation = bx::normalize(bx::mul(bx::mul(qYaw, qPitch), t.rotation));
            });
    }

    m_ecs.progress();
}

void Engine::renderScene() {
    bgfx::setViewRect(kSceneView, 0, 0,
        (uint16_t)m_width, (uint16_t)m_height);

    // Camera matrices
    const float cp = std::cos(m_camera.pitch);
    const float sp = std::sin(m_camera.pitch);
    const float cy = std::cos(m_camera.yaw);
    const float sy = std::sin(m_camera.yaw);

    const bx::Vec3 target = {
        m_camera.position.x + sy * cp,
        m_camera.position.y - sp,
        m_camera.position.z + cy * cp
    };

    float* view = m_view;
    float* proj = m_proj;
    bx::mtxLookAt(view, m_camera.position, target);
    bx::mtxProj(proj, m_fov,
        (float)m_width / (float)m_height,
        0.1f, 1000.0f,
        bgfx::getCaps()->homogeneousDepth);

    bgfx::setViewTransform(kSceneView, view, proj);
    bgfx::touch(kSceneView);

    // Frustum planes (column-major, [0,1] NDC)
    float vp[16];
    bx::mtxMul(vp, view, proj);

    struct Plane { float a, b, c, d; };
    auto normPlane = [](float a, float b, float c, float d) -> Plane {
        float l = std::sqrt(a*a + b*b + c*c);
        return l > 1e-6f ? Plane{a/l,b/l,c/l,d/l} : Plane{};
    };
    Plane planes[6] = {
        normPlane(vp[3]+vp[0], vp[7]+vp[4], vp[11]+vp[8],  vp[15]+vp[12]),
        normPlane(vp[3]-vp[0], vp[7]-vp[4], vp[11]-vp[8],  vp[15]-vp[12]),
        normPlane(vp[3]+vp[1], vp[7]+vp[5], vp[11]+vp[9],  vp[15]+vp[13]),
        normPlane(vp[3]-vp[1], vp[7]-vp[5], vp[11]-vp[9],  vp[15]-vp[13]),
        normPlane(vp[2],       vp[6],        vp[10],         vp[14]),
        normPlane(vp[3]-vp[2], vp[7]-vp[6], vp[11]-vp[10], vp[15]-vp[14]),
    };

    m_ecs.query_builder<const Transform, const MeshRenderer>()
        .build()
        .each([&](flecs::entity, const Transform& t, const MeshRenderer& mr) {
            const Mesh* mesh = m_assets.getMesh(mr.mesh);
            if (!mesh) return;

            // Frustum cull
            if (mesh->hasBounds()) {
                const bx::Vec3 c  = mesh->boundsCenter();
                const bx::Vec3 sz = mesh->boundsSize();
                const float maxS  = std::max({t.scale.x, t.scale.y, t.scale.z});
                const float r     = bx::length(sz) * 0.5f * maxS;
                float m[16]; t.getMatrix(m);
                const float wx = m[0]*c.x + m[4]*c.y + m[8]*c.z  + m[12];
                const float wy = m[1]*c.x + m[5]*c.y + m[9]*c.z  + m[13];
                const float wz = m[2]*c.x + m[6]*c.y + m[10]*c.z + m[14];
                for (const Plane& p : planes)
                    if (p.a*wx + p.b*wy + p.c*wz + p.d < -r) return;
            }

            // Material binding
            const Material* mat = mesh->material.valid()
                ? m_ctx->materials.getMaterial(mesh->material) : nullptr;
            const Texture*  tex = (mat && mat->hasTexture())
                ? m_ctx->textures.getTexture(mat->baseColorTexture) : nullptr;

            float params[4] = {tex ? 1.0f : 0.0f, 0, 0, 0};
            float factor[4] = {1,1,1,1};
            if (mat) {
                factor[0] = mat->baseColorFactor[0]; factor[1] = mat->baseColorFactor[1];
                factor[2] = mat->baseColorFactor[2]; factor[3] = mat->baseColorFactor[3];
            }
            bgfx::setUniform(m_uParams,      params);
            bgfx::setUniform(m_uColorFactor, factor);
            bgfx::setTexture(0, m_sBaseColor, tex ? tex->handle : m_whiteTex);

            float model[16]; t.getMatrix(model);
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, mesh->vbh);
            bgfx::setIndexBuffer(mesh->ibh);

            const uint64_t state = mesh->doubleSided
                ? (BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|
                   BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS|BGFX_STATE_MSAA)
                : BGFX_STATE_DEFAULT;
            bgfx::setState(state);
            bgfx::submit(kSceneView, m_program);
        });
}

void Engine::shutdown() {
    if (m_editorLayer) { m_editorLayer->shutdown(); m_editorLayer.reset(); }

    m_materials.clear();
    m_textures.clear();
    m_assets.clear();

    shutdownRenderer();
    shutdownPlatform();
}

void Engine::shutdownRenderer() {
    imguiShutdown();
    if (bgfx::isValid(m_program))      bgfx::destroy(m_program);
    if (bgfx::isValid(m_sBaseColor))   bgfx::destroy(m_sBaseColor);
    if (bgfx::isValid(m_uParams))      bgfx::destroy(m_uParams);
    if (bgfx::isValid(m_uColorFactor)) bgfx::destroy(m_uColorFactor);
    if (bgfx::isValid(m_whiteTex))     bgfx::destroy(m_whiteTex);
    bgfx::shutdown();
}

void Engine::shutdownPlatform() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}
