#include "render/renderer.h"

#include <cstdio>
#include <cstdint>
#include <utility>
#include <cmath>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "core/transform.h"
#include "core/transform_utils.h"        // getWorldMatrix
#include "components/mesh_renderer.h"
#include "components/light.h"

#include "render/forward_pipeline.h"     // ForwardPipeline + compiled shader bins (single TU)
#include "components/skinned_mesh.h"
#include "animation/skeleton_registry.h"

Renderer::Renderer()  = default;
Renderer::~Renderer() = default;

bool Renderer::init(void* nwh, int width, int height,
                    flecs::world& editorWorld,
                    AssetRegistry& assets, TextureRegistry& textures, MaterialRegistry& materials,
                    SkeletonRegistry& skeletons) {
    m_editorWorld = &editorWorld;
    m_assets      = &assets;
    m_textures    = &textures;
    m_materials   = &materials;
    m_skeletons   = &skeletons;

    // Must precede bgfx::init to select single-threaded mode (otherwise bgfx
    // spins a render thread that races with platform data / a null window).
    bgfx::renderFrame();

    bgfx::Init init;
    // Let bgfx pick the best backend for this platform:
    //   macOS  → Metal    Windows → Direct3D11/12    Linux → Vulkan/OpenGL
#if defined(__APPLE__)
    init.type              = bgfx::RendererType::Metal;
#elif defined(_WIN32)
    init.type              = bgfx::RendererType::Direct3D12;
#else
    init.type              = bgfx::RendererType::Vulkan;
#endif
    init.platformData.nwh  = nwh;
    init.resolution.width  = (uint32_t)width;
    init.resolution.height = (uint32_t)height;
    m_backW = width; m_backH = height;
    init.resolution.reset  = BGFX_RESET_VSYNC;
    if (!bgfx::init(init)) {
        std::printf("[Renderer] bgfx init failed\n");
        return false;
    }

    createSceneFB(width, height);

    static const uint8_t kFlatNorm[4] = {128, 128, 255, 255};
    m_flatNormalTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::copy(kFlatNorm, 4));

    static const uint32_t kWhite = 0xFFFFFFFFu;
    m_whiteTex = bgfx::createTexture2D(1, 1, false, 1,
        bgfx::TextureFormat::RGBA8, 0, bgfx::makeRef(&kWhite, 4));

    m_itemQuery  = editorWorld.query_builder<const Transform, const MeshRenderer>().build();
    m_lightQuery = editorWorld.query_builder<const Transform, const Light>().build();

    if (!m_pipeline) m_pipeline = std::make_unique<ForwardPipeline>();
    RenderContext rc = makeContext();
    m_pipeline->onAttach(rc);

    m_initialized = true;
    return true;
}

void Renderer::setPipeline(std::unique_ptr<IRenderPipeline> pipeline) {
    if (m_initialized && m_pipeline) m_pipeline->onDetach();
    m_pipeline = std::move(pipeline);
    if (m_initialized && m_pipeline) {
        RenderContext rc = makeContext();
        m_pipeline->onAttach(rc);
    }
}

void Renderer::resize(int w, int h) {
    m_backW = w; m_backH = h;
    bgfx::reset((uint32_t)w, (uint32_t)h, BGFX_RESET_VSYNC);
}

void Renderer::createSceneFB(int w, int h) {
    if (bgfx::isValid(m_sceneFB))       bgfx::destroy(m_sceneFB);
    if (bgfx::isValid(m_sceneColorTex)) bgfx::destroy(m_sceneColorTex);
    if (bgfx::isValid(m_sceneDepthTex)) bgfx::destroy(m_sceneDepthTex);
    // The game FB follows the scene FB size — destroy it so renderGameView
    // recreates it at the new size. It must be DESTROYED, not just forgotten:
    // forgetting the handles leaked an FB + two textures per resize, and a
    // continuous Scene View drag exhausted the bgfx texture pool (handle
    // 65535 / "Invalid texture attachment" crash). bgfx::reset() never
    // invalidates user-created handles, so destroying here is safe.
    if (bgfx::isValid(m_gameFB))       bgfx::destroy(m_gameFB);
    if (bgfx::isValid(m_gameColorTex)) bgfx::destroy(m_gameColorTex);
    if (bgfx::isValid(m_gameDepthTex)) bgfx::destroy(m_gameDepthTex);
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
    std::printf("[Renderer] Scene FB: %dx%d\n", w, h);
}

void Renderer::renderScene(const float view[16], const float proj[16]) {
    RenderTarget target;
    target.fb         = m_sceneFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { 0.102f, 0.102f, 0.102f, 1.0f };
    target.clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;

    RenderView    rv = buildView(*m_editorWorld, view, proj, target, kSceneView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void Renderer::renderGameView(const float view[16], const float proj[16],
                              const float clearColor[4], flecs::world* gameWorld) {
    if (!bgfx::isValid(m_gameFB)) {
        const uint16_t W = (uint16_t)m_sceneW, H = (uint16_t)m_sceneH;
        m_gameColorTex = bgfx::createTexture2D(W, H, false, 1,
            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
        m_gameDepthTex = bgfx::createTexture2D(W, H, false, 1,
            bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
        // Defensive: if texture allocation ever fails, skip the game view
        // this frame instead of asserting inside createFrameBuffer.
        if (!bgfx::isValid(m_gameColorTex) || !bgfx::isValid(m_gameDepthTex)) {
            if (bgfx::isValid(m_gameColorTex)) bgfx::destroy(m_gameColorTex);
            if (bgfx::isValid(m_gameDepthTex)) bgfx::destroy(m_gameDepthTex);
            m_gameColorTex = BGFX_INVALID_HANDLE;
            m_gameDepthTex = BGFX_INVALID_HANDLE;
            return;
        }
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

    flecs::world& world = gameWorld ? *gameWorld : *m_editorWorld;
    RenderView    rv = buildView(world, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void Renderer::renderToBackbuffer(const float view[16], const float proj[16],
                                  const float clearColor[4], flecs::world* world) {
    RenderTarget target;
    target.fb         = BGFX_INVALID_HANDLE; // bgfx: invalid FB = backbuffer
    target.w          = (uint16_t)m_backW;
    target.h          = (uint16_t)m_backH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;

    flecs::world& w  = world ? *world : *m_editorWorld;
    RenderView    rv = buildView(w, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

RenderView Renderer::buildView(flecs::world& world, const float view[16],
                               const float proj[16], const RenderTarget& target,
                               bgfx::ViewId baseViewId) {
    RenderView rv;
    rv.view       = Mat4::from(view);
    rv.proj       = Mat4::from(proj);
    rv.target     = target;
    rv.baseViewId = baseViewId;

    rv.camPos = { -(view[12]*view[0] + view[13]*view[1] + view[14]*view[2]),
                  -(view[12]*view[4] + view[13]*view[5] + view[14]*view[6]),
                  -(view[12]*view[8] + view[13]*view[9] + view[14]*view[10]),
                  1.0f };

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

    auto extractItem = [&](flecs::entity e, const Transform&, const MeshRenderer& mr) {
        const Mesh* mesh = m_assets->getMesh(mr.mesh);
        if (!mesh) return;
        RenderItem it;
        getWorldMatrix(e, it.model.m);
        it.mesh = mesh;
        MaterialHandle mh = mr.materialOverride.valid()
                            ? mr.materialOverride : mesh->material;
        it.mat = mh.valid() ? m_materials->getMaterial(mh) : nullptr;
        it.tex = (it.mat && it.mat->hasTexture())
                 ? m_textures->getTexture(it.mat->baseColorTexture) : nullptr;
        it.meshKey = mr.mesh.id;
        it.matKey  = mh.id;

        // Skinned mesh: pass the bone palette to the pipeline
        const SkinnedMesh* skin = e.try_get<SkinnedMesh>();
        if (skin && skin->hasSkinMatrices && m_skeletons) {
            const Skeleton* skel = m_skeletons->get(skin->skeleton);
            if (skel) {
                it.boneMatrices = skin->skinMatrices;
                it.boneCount    = skel->boneCount();
            }
        }

        m_items.push_back(it);
    };
    auto extractLight = [&](flecs::entity e, const Transform&, const Light& lc) {
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
        li.castShadows  = lc.castShadows;
        m_lights.push_back(li);
    };

    m_items.clear();
    m_lights.clear();
    if (&world == m_editorWorld) {              // editor world: cached queries
        m_itemQuery.each(extractItem);
        m_lightQuery.each(extractLight);
    } else {                                    // Play snapshot: cached per world
        m_gameItemQuery.get(world).each(extractItem);
        m_gameLightQuery.get(world).each(extractLight);
    }

    rv.items   = { m_items.data(),  m_items.size() };
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}

RenderContext Renderer::makeContext() {
    RenderContext rc{ *m_assets, *m_textures, *m_materials };
    rc.whiteTex      = m_whiteTex;
    rc.flatNormalTex = m_flatNormalTex;
    rc.viewCursor    = &m_viewCursor;
    rc.shadowViewId  = kShadowView;
    rc.debugDraw     = m_debugDraw;
    return rc;
}

void Renderer::resetWorldCaches() {
    m_gameItemQuery.reset();
    m_gameLightQuery.reset();
}

void Renderer::shutdown() {
    if (m_pipeline) m_pipeline->onDetach();
    if (bgfx::isValid(m_sceneFB))       bgfx::destroy(m_sceneFB);
    if (bgfx::isValid(m_sceneColorTex)) bgfx::destroy(m_sceneColorTex);
    if (bgfx::isValid(m_sceneDepthTex)) bgfx::destroy(m_sceneDepthTex);
    if (bgfx::isValid(m_gameFB))        bgfx::destroy(m_gameFB);
    if (bgfx::isValid(m_gameColorTex))  bgfx::destroy(m_gameColorTex);
    if (bgfx::isValid(m_gameDepthTex))  bgfx::destroy(m_gameDepthTex);
    if (bgfx::isValid(m_flatNormalTex)) bgfx::destroy(m_flatNormalTex);
    if (bgfx::isValid(m_whiteTex))      bgfx::destroy(m_whiteTex);
    bgfx::shutdown();
    m_initialized = false;
}
