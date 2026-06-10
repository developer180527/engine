#include "runtime/runtime.h"
#include "runtime/glfw_platform.h"
#include "runtime/logger.h"

#include <cstdio>
#include <utility>

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
    return init(cfg, std::make_unique<GlfwPlatform>());
}

bool EngineRuntime::init(const EngineConfig& cfg,
                         std::unique_ptr<IPlatform> platform) {
    m_width = cfg.width; m_height = cfg.height; m_fov = cfg.fov;

    // Project loads first so the window title can default to its name.
    m_project = cfg.projectRoot.empty()
        ? ProjectContext::autoDetect()
        : ProjectContext::load(cfg.projectRoot);
    LOG_INFO("Project", "Opened: %s", m_project.projectRoot.string().c_str());

    const std::string title = cfg.title.empty() ? m_project.name : cfg.title;
    m_platform = std::move(platform);
    if (!m_platform->init({title, cfg.width, cfg.height})) return false;
    if (!initRenderer(cfg))  return false;
    if (!initSystems(cfg))   return false;
    buildDefaultScene();
    return true;
}

bool EngineRuntime::initRenderer(const EngineConfig& cfg) {
    void* nwh = m_platform->nativeWindowHandle();
    if (!nwh) {
        // Headless platform — no GPU. ECS, assets, animation, physics and
        // scripting still run; render entry points become no-ops.
        m_headless = true;
        return true;
    }
    return m_renderer.init(nwh, cfg.width, cfg.height,
                           m_ecs, m_assets, m_textures, m_materials,
                           m_skeletons);
}

bool EngineRuntime::initSystems(const EngineConfig& cfg) {
    m_importers.registerImporter(std::make_unique<GltfImporter>());
    m_importers.registerImporter(std::make_unique<AssimpImporter>());

    // Asset database — open + scan so handles resolve from the first frame.
    // CookService (editor/CLI tooling) opens its own write connection (WAL).
    const auto cacheRoot = m_project.projectRoot / ".cache";
    if (cfg.openAssetDatabase && !m_project.projectRoot.empty()) {
        const auto dbPath = cacheRoot / "registry.db";
        if (m_assetLib.open(dbPath)) {
            int n = 0;
            if (std::filesystem::exists(m_project.assetsRoot))
                n = m_assetLib.scan(m_project.assetsRoot, m_project.projectRoot);
            LOG_INFO("AssetLib", "Registry ready — %zu asset(s), %d new/updated",
                     m_assetLib.all().size(), n);
        } else {
            LOG_WARN("AssetLib", "Could not open registry at: %s",
                     dbPath.string().c_str());
        }
    }

    // AssetService — async mesh/texture loading for scripts + scene streaming.
    m_assetService = std::make_unique<AssetService>(AssetService::Config{
        m_assets, m_textures, m_materials});
    m_assetService->setAssetLib(&m_assetLib);
    m_assetService->setProjectRoot(m_project.projectRoot);

    // SceneService — built on top of AssetService for binary scene loading.
    m_sceneService = std::make_unique<SceneService>(SceneService::Config{
        *m_assetService, m_assets, m_textures, m_materials, m_ecs, &m_primitives});
    m_sceneService->setCacheRoot(cacheRoot);

    m_ctx = std::make_unique<RuntimeContext>(RuntimeContext{
        m_ecs, m_assets, m_textures,
        m_materials, m_project, m_importers});
    m_primitives.init(m_assets);
    m_ctx->assetLib      = &m_assetLib;
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
    if (m_headless) return; // GPU buffers — nothing to build without a device

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

bool EngineRuntime::frameBegin(float& dt) {
    if (!m_platform || m_platform->shouldClose()) return false;

    m_platform->pollEvents();

    // While minimized (zero-size framebuffer), block on events instead of
    // spinning. Headless platforms report 0x0 too but never resize — skip
    // the wait for them so servers don't stall.
    if (!m_headless) {
        int fbw = 0, fbh = 0;
        m_platform->framebufferSize(fbw, fbh);
        while ((fbw <= 0 || fbh <= 0) && !m_platform->shouldClose()) {
            m_platform->waitEvents(0.1);
            m_platform->pollEvents();
            m_platform->framebufferSize(fbw, fbh);
        }
        if (m_platform->shouldClose()) return false;
        resize(fbw, fbh); // no-op when unchanged
    }

    // Clamped frame delta — first frame gets 0 so a long init doesn't
    // produce a giant simulation step.
    const auto now = std::chrono::steady_clock::now();
    dt = m_firstFrame ? 0.0f
        : std::chrono::duration<float>(now - m_lastFrameTime).count();
    if (dt > 0.05f) dt = 0.05f;
    m_lastFrameTime = now;
    m_firstFrame    = false;

    // Drain async asset uploads (main-thread GPU upload)
    if (m_assetService) m_assetService->drainUploads();

    return true;
}

void EngineRuntime::frameEnd() {
    if (!m_headless) bgfx::frame();
}

void EngineRuntime::run(const std::function<void(float)>& frame) {
    float dt = 0.0f;
    while (frameBegin(dt)) {
        frame(dt);
        frameEnd();
    }
}

void EngineRuntime::resize(int w, int h) {
    if (w == m_width && h == m_height) return;
    m_width = w; m_height = h;
    if (!m_headless) m_renderer.resize(w, h);
}

void EngineRuntime::tick(float dt, const float view[16],
                         const float proj[16], bool pauseSystems) {
    tickSystems(dt, pauseSystems);
    if (!m_headless) m_renderer.renderScene(view, proj);
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
    m_plugins.detachAll(); // plugins may hold services — detach before teardown
    m_clips.clear();
    m_skeletons.clear();
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    if (!m_headless) m_renderer.shutdown();
    if (m_platform) { m_platform->shutdown(); m_platform.reset(); }
}
