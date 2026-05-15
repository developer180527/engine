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

#include "metal/vs_triangle.sc.bin.h"
#include "metal/fs_triangle.sc.bin.h"

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

    bgfx::setViewClear(kSceneView,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1a1a1aff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0, bgfx::BackbufferRatio::Equal);

    m_program = bgfx::createProgram(
        bgfx::createShader(bgfx::makeRef(vs_triangle_mtl, sizeof(vs_triangle_mtl))),
        bgfx::createShader(bgfx::makeRef(fs_triangle_mtl, sizeof(fs_triangle_mtl))),
        true);
    if (!bgfx::isValid(m_program)) {
        std::printf("[Runtime] Shader program failed\n");
        return false;
    }

    m_sBaseColor   = bgfx::createUniform("s_baseColor",   bgfx::UniformType::Sampler);
    m_uParams      = bgfx::createUniform("u_params",      bgfx::UniformType::Vec4);
    m_uColorFactor = bgfx::createUniform("u_colorFactor", bgfx::UniformType::Vec4);
    m_uLightDir    = bgfx::createUniform("u_lightDir",    bgfx::UniformType::Vec4);
    m_uLightParams = bgfx::createUniform("u_lightParams", bgfx::UniformType::Vec4);

    static const uint32_t kWhite = 0xFFFFFFFFu;
    m_whiteTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::makeRef(&kWhite, 4));

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
    bgfx::setViewRect(kSceneView, 0, 0, bgfx::BackbufferRatio::Equal);
}

void EngineRuntime::tick(float dt, const float view[16],
                         const float proj[16], bool pauseSystems) {
    bgfx::setViewTransform(kSceneView, view, proj);
    bgfx::touch(kSceneView);
    // Light: directional sun from upper-right-front, 30% ambient
    const float kLightDir[4]    = { 0.6f, 0.8f, 0.4f, 0.0f }; // toward light
    const float kLightParams[4] = { 0.25f, 0.0f, 0.0f, 0.0f }; // ambient
    bgfx::setUniform(m_uLightDir,    kLightDir);
    bgfx::setUniform(m_uLightParams, kLightParams);
    tickSystems(dt, pauseSystems);
    renderScene(view, proj);
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

void EngineRuntime::renderScene(const float view[16], const float proj[16]) {
    // Frustum planes (column-major, [0,1] NDC depth — Metal convention)
    float vp[16];
    bx::mtxMul(vp, view, proj);

    struct Plane { float a, b, c, d; };
    auto mkPlane = [](float a, float b, float c, float d) -> Plane {
        float l = std::sqrt(a*a + b*b + c*c);
        return l > 1e-6f ? Plane{a/l, b/l, c/l, d/l} : Plane{};
    };
    const Plane planes[6] = {
        mkPlane(vp[3]+vp[0], vp[7]+vp[4], vp[11]+vp[8],  vp[15]+vp[12]),
        mkPlane(vp[3]-vp[0], vp[7]-vp[4], vp[11]-vp[8],  vp[15]-vp[12]),
        mkPlane(vp[3]+vp[1], vp[7]+vp[5], vp[11]+vp[9],  vp[15]+vp[13]),
        mkPlane(vp[3]-vp[1], vp[7]-vp[5], vp[11]-vp[9],  vp[15]-vp[13]),
        mkPlane(vp[2],        vp[6],        vp[10],         vp[14]),
        mkPlane(vp[3]-vp[2], vp[7]-vp[6], vp[11]-vp[10], vp[15]-vp[14]),
    };

    m_ecs.query_builder<const Transform, const MeshRenderer>().build()
        .each([&](flecs::entity, const Transform& t, const MeshRenderer& mr) {
            const Mesh* mesh = m_assets.getMesh(mr.mesh);
            if (!mesh) return;

            // Sphere-frustum cull
            if (mesh->hasBounds()) {
                const bx::Vec3 c   = mesh->boundsCenter();
                const float    maxS = std::max({t.scale.x, t.scale.y, t.scale.z});
                const float    r    = bx::length(mesh->boundsSize()) * 0.5f * maxS;
                float m[16]; t.getMatrix(m);
                const float wx = m[0]*c.x+m[4]*c.y+m[8]*c.z +m[12];
                const float wy = m[1]*c.x+m[5]*c.y+m[9]*c.z +m[13];
                const float wz = m[2]*c.x+m[6]*c.y+m[10]*c.z+m[14];
                for (const Plane& p : planes)
                    if (p.a*wx+p.b*wy+p.c*wz+p.d < -r) return;
            }

            // Material + texture binding
            const Material* mat = mesh->material.valid()
                ? m_ctx->materials.getMaterial(mesh->material) : nullptr;
            const Texture*  tex = (mat && mat->hasTexture())
                ? m_ctx->textures.getTexture(mat->baseColorTexture) : nullptr;

            float params[4] = {tex ? 1.0f : 0.0f, 0, 0, 0};
            float factor[4] = {1, 1, 1, 1};
            if (mat) {
                factor[0] = mat->baseColorFactor[0];
                factor[1] = mat->baseColorFactor[1];
                factor[2] = mat->baseColorFactor[2];
                factor[3] = mat->baseColorFactor[3];
            }
            bgfx::setUniform(m_uParams,      params);
            bgfx::setUniform(m_uColorFactor, factor);
            bgfx::setTexture(0, m_sBaseColor, tex ? tex->handle : m_whiteTex);

            float model[16]; t.getMatrix(model);
            bgfx::setTransform(model);
            bgfx::setVertexBuffer(0, mesh->vbh);
            bgfx::setIndexBuffer(mesh->ibh);

            const uint64_t state = mesh->doubleSided
                ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_MSAA)
                : BGFX_STATE_DEFAULT;
            bgfx::setState(state);
            bgfx::submit(kSceneView, m_program);
        });
}

void EngineRuntime::shutdown() {
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    shutdownRenderer();
    shutdownPlatform();
}

void EngineRuntime::shutdownRenderer() {
    if (bgfx::isValid(m_program))      bgfx::destroy(m_program);
    if (bgfx::isValid(m_sBaseColor))   bgfx::destroy(m_sBaseColor);
    if (bgfx::isValid(m_uParams))      bgfx::destroy(m_uParams);
    if (bgfx::isValid(m_uColorFactor)) bgfx::destroy(m_uColorFactor);
    if (bgfx::isValid(m_uLightDir))    bgfx::destroy(m_uLightDir);
    if (bgfx::isValid(m_uLightParams)) bgfx::destroy(m_uLightParams);
    if (bgfx::isValid(m_whiteTex))     bgfx::destroy(m_whiteTex);
    bgfx::shutdown();
}

void EngineRuntime::shutdownPlatform() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; }
    glfwTerminate();
}
