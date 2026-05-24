#include <vector>
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

    createSceneFB(cfg.width, cfg.height);

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
    m_uLightColor  = bgfx::createUniform("u_lightColor",  bgfx::UniformType::Vec4);
    m_uCamPos      = bgfx::createUniform("u_camPos",      bgfx::UniformType::Vec4);
    m_sNormalMap   = bgfx::createUniform("s_normalMap",   bgfx::UniformType::Sampler);
    // Flat normal (0.5,0.5,1) → (0,0,1) in tangent space — default when no normal map
    static const uint8_t kFlatNorm[4] = {128, 128, 255, 255};
    m_flatNormalTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::copy(kFlatNorm, 4));

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
}

void EngineRuntime::tick(float dt, const float view[16],
                         const float proj[16], bool pauseSystems) {
    bgfx::setViewFrameBuffer(kSceneView, m_sceneFB);
    bgfx::setViewRect(kSceneView, 0, 0, (uint16_t)m_sceneW, (uint16_t)m_sceneH);
    bgfx::setViewTransform(kSceneView, view, proj);
    bgfx::touch(kSceneView);
    // Light: directional sun from upper-right-front, 30% ambient
    const float kLightDir[4]    = { 0.6f, 0.8f, 0.4f, 0.0f }; // toward light
    const float kLightParams[4] = { 0.25f, 0.0f, 0.0f, 0.0f }; // x=ambient
    const float kLightColor[4]  = { 1.0f, 0.98f, 0.92f, 2.2f }; // rgb + intensity
    // Camera world position extracted from column-major view matrix.
    // view[12-14] = translation column = -R*eye, so eye = -R^T * T
    // bx view matrix is row-major: T=(view[12],view[13],view[14])
    // cam = -(T · col_i_of_R) where col i = (view[i*4], view[i*4+1], view[i*4+2])
    const float camPos[4] = {
        -(view[12]*view[0] + view[13]*view[1] + view[14]*view[2]),
        -(view[12]*view[4] + view[13]*view[5] + view[14]*view[6]),
        -(view[12]*view[8] + view[13]*view[9] + view[14]*view[10]),
        1.0f };
    bgfx::setUniform(m_uLightDir,    kLightDir);
    bgfx::setUniform(m_uLightParams, kLightParams);
    bgfx::setUniform(m_uLightColor,  kLightColor);
    bgfx::setUniform(m_uCamPos,      camPos);
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
    // ----------------------------------------------------------------
    // 1. Frustum planes
    // ----------------------------------------------------------------
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

    // ----------------------------------------------------------------
    // 2. Collect visible renderables
    // ----------------------------------------------------------------
    struct Renderable {
        float           model[16];
        const Mesh*     mesh;
        const Material* mat;
        const Texture*  tex;
        uint32_t        meshIdx;  // MeshHandle.idx for instancing key
        uint32_t        matIdx;   // MaterialHandle.idx for batching key
    };

    std::vector<Renderable> visible;
    visible.reserve(64);

    m_ecs.query_builder<const Transform, const MeshRenderer>().build()
        .each([&](flecs::entity, const Transform& t, const MeshRenderer& mr) {
            const Mesh* mesh = m_assets.getMesh(mr.mesh);
            if (!mesh) return;

            if (mesh->hasBounds()) {
                const bx::Vec3 c    = mesh->boundsCenter();
                const float    maxS = std::max({t.scale.x, t.scale.y, t.scale.z});
                const float    r    = bx::length(mesh->boundsSize()) * 0.5f * maxS;
                float m[16]; t.getMatrix(m);
                const float wx = m[0]*c.x+m[4]*c.y+m[8]*c.z +m[12];
                const float wy = m[1]*c.x+m[5]*c.y+m[9]*c.z +m[13];
                const float wz = m[2]*c.x+m[6]*c.y+m[10]*c.z+m[14];
                for (const Plane& p : planes)
                    if (p.a*wx+p.b*wy+p.c*wz+p.d < -r) return;
            }

            Renderable r;
            t.getMatrix(r.model);
            r.mesh    = mesh;
            r.mat     = mesh->material.valid()
                        ? m_ctx->materials.getMaterial(mesh->material) : nullptr;
            r.tex     = (r.mat && r.mat->hasTexture())
                        ? m_ctx->textures.getTexture(r.mat->baseColorTexture) : nullptr;
            r.meshIdx = mr.mesh.id;
            r.matIdx  = mesh->material.id;
            visible.push_back(r);
        });

    // ----------------------------------------------------------------
    // 3. Sort by (meshIdx, matIdx) — groups instancable entities together
    //    and minimises material state switches between groups
    // ----------------------------------------------------------------
    std::sort(visible.begin(), visible.end(), [](const Renderable& a, const Renderable& b) {
        if (a.meshIdx != b.meshIdx) return a.meshIdx < b.meshIdx;
        return a.matIdx < b.matIdx;
    });

    // ----------------------------------------------------------------
    // 4. Submit — instanced for groups > 1, single draw otherwise
    // ----------------------------------------------------------------
    size_t i = 0;
    while (i < visible.size()) {
        const Renderable& first = visible[i];

        // Count how many consecutive renderables share the same mesh+mat
        size_t j = i + 1;
        while (j < visible.size()
               && visible[j].meshIdx == first.meshIdx
               && visible[j].matIdx  == first.matIdx) {
            ++j;
        }
        const uint32_t groupSize = (uint32_t)(j - i);

        // Bind material state once per group
        float roughness = first.mat ? first.mat->roughness : 0.7f;
        float metallic  = first.mat ? first.mat->metallic  : 0.0f;
        const Texture* nmTex = (first.mat && first.mat->normalMapTexture.valid())
            ? m_ctx->textures.getTexture(first.mat->normalMapTexture) : nullptr;
        float params[4] = {first.tex?1.0f:0.0f, roughness, metallic, nmTex?1.0f:0.0f};
        float factor[4] = {1, 1, 1, 1};
        if (first.mat) {
            factor[0] = first.mat->baseColorFactor[0];
            factor[1] = first.mat->baseColorFactor[1];
            factor[2] = first.mat->baseColorFactor[2];
            factor[3] = first.mat->baseColorFactor[3];
        }
        bgfx::setUniform(m_uParams,      params);
        bgfx::setUniform(m_uColorFactor, factor);
        bgfx::setTexture(0, m_sBaseColor,
                         first.tex ? first.tex->handle : m_whiteTex);

        const uint64_t state = first.mesh->doubleSided
            ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
               BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS |
               BGFX_STATE_MSAA)
            : BGFX_STATE_DEFAULT;

        // Individual draw calls per group — material state set once above.
        // Instancing removed until shader supports i_data0-3 per-instance
        // model matrix (requires VS rewrite to use u_viewProj + i_data rows).
        for (uint32_t k = 0; k < groupSize; ++k) {
            bgfx::setTransform(visible[i + k].model);
            if (first.mesh->submeshes.empty()) {
                // Single draw — whole IB, mesh.material already bound above
                bgfx::setVertexBuffer(0, first.mesh->vbh);
                bgfx::setIndexBuffer(first.mesh->ibh);
                bgfx::setState(state);
                bgfx::submit(kSceneView, m_program);
            } else {
                for (const auto& sub : first.mesh->submeshes) {
                    bgfx::setUniform(m_uParams,      params);
                    bgfx::setUniform(m_uColorFactor, factor);
                    bgfx::setTexture(0, m_sBaseColor,
                                    first.tex ? first.tex->handle : m_whiteTex);
                    bgfx::setTexture(1, m_sNormalMap,
                                    nmTex ? nmTex->handle : m_flatNormalTex);
                    bgfx::setState(state);
                    bgfx::setTransform(visible[i + k].model);
                    bgfx::setVertexBuffer(0, first.mesh->vbh);
                    bgfx::setIndexBuffer(first.mesh->ibh,
                                        sub.indexOffset, sub.indexCount);
                    bgfx::submit(kSceneView, m_program);
                }
            }
        }
        i = j;
    }
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
    if (bgfx::isValid(m_sceneFB))       bgfx::destroy(m_sceneFB);
    if (bgfx::isValid(m_sceneColorTex)) bgfx::destroy(m_sceneColorTex);
    if (bgfx::isValid(m_sceneDepthTex)) bgfx::destroy(m_sceneDepthTex);
    if (bgfx::isValid(m_uLightColor))   bgfx::destroy(m_uLightColor);
    if (bgfx::isValid(m_uCamPos))       bgfx::destroy(m_uCamPos);
    if (bgfx::isValid(m_sNormalMap))    bgfx::destroy(m_sNormalMap);
    if (bgfx::isValid(m_flatNormalTex)) bgfx::destroy(m_flatNormalTex);
    if (bgfx::isValid(m_program))       bgfx::destroy(m_program);
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
