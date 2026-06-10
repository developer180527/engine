#include "runtime/runtime.h"

#include <cstdio>
#include <utility>

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
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include "render/vertex.h"
#include "render/primitive_cube.h"
#include "render/mesh.h"
#include "io/gltf_importer.h"
#include "io/assimp_importer.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "core/transform.h"

EngineRuntime::EngineRuntime()  = default;
EngineRuntime::~EngineRuntime() = default;

bool EngineRuntime::init(const EngineConfig& cfg) {
    m_width = cfg.width; m_height = cfg.height; m_fov = cfg.fov;
    if (!initPlatform(cfg))  return false;
    if (!initRenderer(cfg))  return false;
    if (!initSystems())      return false;
    buildDefaultScene();
    return true;
}

bool EngineRuntime::initPlatform(const EngineConfig& cfg) {
    if (!glfwInit()) {
        std::printf("[Runtime] GLFW init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(cfg.width, cfg.height,
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::printf("[Runtime] Window creation failed\n");
        return false;
    }
    return true;
}

// Retrieve the platform-specific native window handle that bgfx needs.
static void* getNativeWindowHandle(GLFWwindow* window) {
#if defined(__APPLE__)
    return glfwGetCocoaWindow(window);
#elif defined(_WIN32)
    return glfwGetWin32Window(window);
#elif defined(__linux__)
    // Prefer Wayland when available (GLFW 3.4+); fall back to X11.
    #if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return (void*)glfwGetWaylandWindow(window);
    #endif
    return (void*)glfwGetX11Window(window);
#else
    #error "Unsupported platform — add native window handle retrieval"
#endif
}

bool EngineRuntime::initRenderer(const EngineConfig& cfg) {
    return m_renderer.init(getNativeWindowHandle(m_window),
                           cfg.width, cfg.height,
                           m_ecs, m_assets, m_textures, m_materials,
                           m_skeletons);
}

bool EngineRuntime::initSystems() {
    m_project = ProjectContext::autoDetect();
    std::printf("[Runtime] Assets root: %s\n",
                m_project.assetsRoot.string().c_str());

    m_importers.registerImporter(std::make_unique<GltfImporter>());
    m_importers.registerImporter(std::make_unique<AssimpImporter>());

    // AssetService — created with registries; assetLib + projectRoot
    // are wired later from main.cpp via setAssetLib() / setProjectRoot().
    m_assetService = std::make_unique<AssetService>(AssetService::Config{
        m_assets, m_textures, m_materials});

    // SceneService — built on top of AssetService for binary scene loading.
    // cacheRoot is wired later from main.cpp alongside AssetService config.
    m_sceneService = std::make_unique<SceneService>(SceneService::Config{
        *m_assetService, m_assets, m_textures, m_materials, m_ecs, &m_primitives});

    m_ctx = std::make_unique<RuntimeContext>(RuntimeContext{
        m_ecs, m_assets, m_textures,
        m_materials, m_project, m_importers});
    m_primitives.init(m_assets);
    m_ctx->primitives    = &m_primitives;
    m_ctx->assetService  = m_assetService.get();
    m_ctx->sceneService  = m_sceneService.get();
    m_ctx->skeletons     = &m_skeletons;
    m_ctx->clips         = &m_clips;

    // Gameplay-tick query (editor world) — render queries live in Renderer.
    m_spinnerQuery = m_ecs.query_builder<Transform, const Spinner>().build();

    // Animation system — samples clips and writes bone palettes each frame.
    m_animatorSystem.init(m_ecs, m_skeletons, m_clips);

    return true;
}

void EngineRuntime::buildDefaultScene() {
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::makeRef(primitive_cube::kVertices, sizeof(primitive_cube::kVertices)),
        Vertex::layout());
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
        bgfx::makeRef(primitive_cube::kIndices,  sizeof(primitive_cube::kIndices)));

    Mesh cube(vbh, ibh,
        (uint32_t)(sizeof(primitive_cube::kIndices)
                   / sizeof(primitive_cube::kIndices[0])));
    cube.boundsMin = {-1.0f, -1.0f, -1.0f};
    cube.boundsMax = { 1.0f,  1.0f,  1.0f};

    MeshHandle cubeHandle = m_assets.addMesh(std::move(cube));

    constexpr int   kGrid    = 3;
    constexpr float kSpacing = 5.0f;
    for (int x = 0; x < kGrid; ++x) {
        for (int z = 0; z < kGrid; ++z) {
            char name[32];
            std::snprintf(name, sizeof(name), "Cube (%d,%d)", x, z);
            Transform t;
            t.position = {(x - kGrid/2) * kSpacing, 0.0f,
                          (z - kGrid/2) * kSpacing};
            m_ecs.entity(name)
                .set<Transform>(t)
                .set<MeshRenderer>({cubeHandle})
                .set<Name>({name})
                .set<Spinner>({0.3f, 0.1f});
        }
    }
}

void EngineRuntime::resize(int w, int h) {
    if (w == m_width && h == m_height) return;
    m_width = w; m_height = h;
    m_renderer.resize(w, h);
}

void EngineRuntime::tick(float dt, const float view[16],
                         const float proj[16], bool pauseSystems) {
    tickSystems(dt, pauseSystems);
    m_renderer.renderScene(view, proj);
}

void EngineRuntime::tickSystems(float dt, bool paused) {
    if (!paused) {
        m_spinnerQuery
            .each([dt](flecs::entity, Transform& t, const Spinner& s) {
                const bx::Quaternion qY =
                    bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
                const bx::Quaternion qP =
                    bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);
                t.rotation = bx::normalize(bx::mul(qP, bx::mul(qY, t.rotation)));
            });
    }
    // Animation runs even when gameplay systems are paused — the editor
    // scrubber and preview should always animate.
    m_animatorSystem.tick(dt);
    m_ecs.progress();
}

void EngineRuntime::shutdown() {
    m_clips.clear();
    m_skeletons.clear();
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    m_renderer.shutdown();
    shutdownPlatform();
}

void EngineRuntime::shutdownPlatform() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}
