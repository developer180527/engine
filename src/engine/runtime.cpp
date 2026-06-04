#include <vector>
#include "components/light.h"
#include "render/render_pipeline.h"
#include <cstring>
#include "engine/runtime.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include "render/vertex.h"
#include "render/primitive_cube.h"
#include "render/primitive_library.h"
#include "core/transform_utils.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"
#include "io/gltf_importer.h"
#include "io/assimp_importer.h"
#include "io/asset_storage.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "core/transform.h"

#include "render/forward_pipeline.h"

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

bool EngineRuntime::initRenderer(const EngineConfig& cfg) {
    // Must be called before bgfx::init to select single-threaded mode.
    // Without this, bgfx creates a render thread that races with
    // setPlatformData and sees a null window handle.
    bgfx::renderFrame();

    bgfx::Init init;
    init.type              = bgfx::RendererType::Metal;
    init.platformData.nwh  = glfwGetCocoaWindow(m_window);
    init.resolution.width  = (uint32_t)cfg.width;
    init.resolution.height = (uint32_t)cfg.height;
    init.resolution.reset  = BGFX_RESET_VSYNC;

    if (!bgfx::init(init)) {
        std::printf("[Runtime] bgfx init failed\n");
        return false;
    }

    createSceneFB(cfg.width, cfg.height);

    // Flat normal (0.5,0.5,1) → (0,0,1) in tangent space — default when no normal map
    static const uint8_t kFlatNorm[4] = {128, 128, 255, 255};
    m_flatNormalTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::copy(kFlatNorm, 4));

    static const uint32_t kWhite = 0xFFFFFFFFu;
    m_whiteTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::makeRef(&kWhite, 4));

    m_pipeline = std::make_unique<ForwardPipeline>();
    RenderContext rc = makeContext();
    m_pipeline->onAttach(rc);

    return true;
}

bool EngineRuntime::initSystems() {
    m_project = ProjectContext::autoDetect();
    std::printf("[Runtime] Assets root: %s\n",
                m_project.assetsRoot.string().c_str());

    m_importers.registerImporter(std::make_unique<GltfImporter>());
    m_importers.registerImporter(std::make_unique<AssimpImporter>());

    m_ctx = std::make_unique<RuntimeContext>(RuntimeContext{
        m_ecs, m_assets, m_textures,
        m_materials, m_project, m_importers});
    m_primitives.init(m_assets);
    m_ctx->primitives = &m_primitives;
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
    bgfx::reset((uint32_t)w, (uint32_t)h, BGFX_RESET_VSYNC);
}

void EngineRuntime::tick(float dt, const float view[16],
                         const float proj[16], bool pauseSystems) {
    tickSystems(dt, pauseSystems);

    RenderTarget target;
    target.fb         = m_sceneFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { 0.102f, 0.102f, 0.102f, 1.0f }; // matches old 0x1a1a1a
    target.clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;

    RenderView    rv = buildView(m_ecs, view, proj, target, kSceneView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void EngineRuntime::tickSystems(float dt, bool paused) {
    if (!paused) {
        m_ecs.query_builder<Transform, const Spinner>().build()
            .each([dt](flecs::entity, Transform& t, const Spinner& s) {
                const bx::Quaternion qY =
                    bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
                const bx::Quaternion qP =
                    bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);
                t.rotation = bx::normalize(bx::mul(qP, bx::mul(qY, t.rotation)));
            });
    }
    m_ecs.progress();
}

RenderView EngineRuntime::buildView(flecs::world& world, const float view[16],
                                    const float proj[16], const RenderTarget& target,
                                    bgfx::ViewId baseViewId) {
    RenderView rv;
    rv.view       = Mat4::from(view);
    rv.proj       = Mat4::from(proj);
    rv.target     = target;
    rv.baseViewId = baseViewId;

    // Camera world position from the row-major view matrix.
    rv.camPos = { -(view[12]*view[0] + view[13]*view[1] + view[14]*view[2]),
                  -(view[12]*view[4] + view[13]*view[5] + view[14]*view[6]),
                  -(view[12]*view[8] + view[13]*view[9] + view[14]*view[10]),
                  1.0f };

    // Frustum planes from view*proj.
    float vp[16]; bx::mtxMul(vp, view, proj);
    auto setPlane = [&](int i, float a, float b, float c, float d) {
        float l = std::sqrt(a*a + b*b + c*c); if (l < 1e-6f) l = 1.0f;
        rv.frustum[i][0]=a/l; rv.frustum[i][1]=b/l; rv.frustum[i][2]=c/l; rv.frustum[i][3]=d/l;
    };
    setPlane(0, vp[3]+vp[0], vp[7]+vp[4], vp[11]+vp[8],  vp[15]+vp[12]);
    setPlane(1, vp[3]-vp[0], vp[7]-vp[4], vp[11]-vp[8],  vp[15]-vp[12]);
    setPlane(2, vp[3]+vp[1], vp[7]+vp[5], vp[11]+vp[9],  vp[15]+vp[13]);
    setPlane(3, vp[3]-vp[1], vp[7]-vp[5], vp[11]-vp[9],  vp[15]-vp[13]);
    setPlane(4, vp[2],        vp[6],        vp[10],         vp[14]);
    setPlane(5, vp[3]-vp[2], vp[7]-vp[6], vp[11]-vp[10], vp[15]-vp[14]);

    // Extract renderables (UNCULLED — the pipeline culls).
    m_items.clear();
    world.query_builder<const Transform, const MeshRenderer>().build()
        .each([&](flecs::entity e, const Transform&, const MeshRenderer& mr) {
            const Mesh* mesh = m_assets.getMesh(mr.mesh);
            if (!mesh) return;
            RenderItem it;
            getWorldMatrix(e, it.model.m);
            it.mesh = mesh;
            MaterialHandle mh = mr.materialOverride.valid()
                                ? mr.materialOverride : mesh->material;
            it.mat = mh.valid() ? m_materials.getMaterial(mh) : nullptr;
            it.tex = (it.mat && it.mat->hasTexture())
                     ? m_textures.getTexture(it.mat->baseColorTexture) : nullptr;
            it.meshKey = mr.mesh.id;
            it.matKey  = mh.id;
            m_items.push_back(it);
        });

    m_lights.clear();
    world.query_builder<const Transform, const Light>().build()
        .each([&](flecs::entity e, const Transform&, const Light& lc) {
            float m[16]; getWorldMatrix(e, m);
            LightItem li;
            li.type      = lc.type;
            {
                const bx::Vec3 kc = lc.useTemperature
                    ? kelvinToRGB(lc.temperatureK) : bx::Vec3{ 1.0f, 1.0f, 1.0f };
                li.color = bx::Vec3{ kc.x * lc.color.x, kc.y * lc.color.y, kc.z * lc.color.z };
            }
            li.intensity = lc.intensity;
            li.range     = lc.range;
            li.position  = bx::Vec3{ m[12], m[13], m[14] };
            // light emits along local -Z, so toward-light = +local-Z in world
            li.direction = bx::normalize(bx::Vec3{ -m[8], -m[9], -m[10] });
            li.spotInnerCos = std::cos(lc.spotInner * (3.14159265f / 180.0f));
            li.spotOuterCos = std::cos(lc.spotOuter * (3.14159265f / 180.0f));
            m_lights.push_back(li);
        });

    rv.items   = { m_items.data(),  m_items.size() };
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}

RenderContext EngineRuntime::makeContext() {
    RenderContext rc{ m_assets, m_textures, m_materials };
    rc.whiteTex      = m_whiteTex;
    rc.flatNormalTex = m_flatNormalTex;
    rc.viewCursor    = &m_viewCursor;
    return rc;
}

void EngineRuntime::renderGameView(const float view[16], const float proj[16],
                                   const float clearColor[4],
                                   flecs::world* gameWorld) {
    if (!bgfx::isValid(m_gameFB)) {
        const uint16_t W = (uint16_t)m_sceneW, H = (uint16_t)m_sceneH;
        m_gameColorTex = bgfx::createTexture2D(W, H, false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
        m_gameDepthTex = bgfx::createTexture2D(W, H, false, 1,
            bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
        bgfx::TextureHandle gatt[2] = { m_gameColorTex, m_gameDepthTex };
        m_gameFB = bgfx::createFrameBuffer(2, gatt, false);
    }
    if (!bgfx::isValid(m_gameFB)) return;

    RenderTarget target;
    target.fb         = m_gameFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;

    flecs::world& world = gameWorld ? *gameWorld : m_ecs;
    RenderView    rv = buildView(world, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void EngineRuntime::shutdown() {
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    shutdownRenderer();
    shutdownPlatform();
}

void EngineRuntime::createSceneFB(int w, int h) {
    if (bgfx::isValid(m_sceneFB))       bgfx::destroy(m_sceneFB);
    if (bgfx::isValid(m_sceneColorTex)) bgfx::destroy(m_sceneColorTex);
    if (bgfx::isValid(m_sceneDepthTex)) bgfx::destroy(m_sceneDepthTex);
    // Invalidate game FB handles — they are destroyed separately
    // in shutdownRenderer. Do NOT destroy here; bgfx::reset() may
    // have already invalidated them, causing a double-free crash.
    m_gameFB       = BGFX_INVALID_HANDLE;
    m_gameColorTex = BGFX_INVALID_HANDLE;
    m_gameDepthTex = BGFX_INVALID_HANDLE;

    const uint16_t W = (uint16_t)w, H = (uint16_t)h;
    m_sceneColorTex = bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    m_sceneDepthTex = bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
    bgfx::TextureHandle att[2] = { m_sceneColorTex, m_sceneDepthTex };
    m_sceneFB = bgfx::createFrameBuffer(2, att, false);

    m_sceneW = w; m_sceneH = h;
    bgfx::setViewFrameBuffer(kSceneView, m_sceneFB);
    bgfx::setViewClear(kSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1a1a1aff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0, W, H);
    std::printf("[Runtime] Scene FB: %dx%d\n", w, h);
}

void EngineRuntime::shutdownRenderer() {
    if (m_pipeline) m_pipeline->onDetach();
    if (bgfx::isValid(m_sceneFB))       bgfx::destroy(m_sceneFB);
    if (bgfx::isValid(m_sceneColorTex)) bgfx::destroy(m_sceneColorTex);
    if (bgfx::isValid(m_sceneDepthTex)) bgfx::destroy(m_sceneDepthTex);
    if (bgfx::isValid(m_gameFB))        bgfx::destroy(m_gameFB);
    if (bgfx::isValid(m_gameColorTex))  bgfx::destroy(m_gameColorTex);
    if (bgfx::isValid(m_gameDepthTex))  bgfx::destroy(m_gameDepthTex);
    if (bgfx::isValid(m_flatNormalTex)) bgfx::destroy(m_flatNormalTex);
    if (bgfx::isValid(m_whiteTex))     bgfx::destroy(m_whiteTex);
    bgfx::shutdown();
}

void EngineRuntime::shutdownPlatform() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}
