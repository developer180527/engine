#include "render/renderer.h"

#include <cstdio>
#include <cstdint>
#include <utility>
#include <cmath>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/allocator.h>
#include <bx/math.h>

#include <cstring>

#include "core/memory/mem.h"

// bgfx/bx allocations → Rendering heap. bx funnels everything through one
// realloc-style virtual; the instance must outlive bgfx::shutdown → static.
namespace {
struct BgfxMemAllocator final : bx::AllocatorI {
    void* realloc(void* p, size_t size, size_t align,
                  const char*, uint32_t) override {
        if (size == 0) { mem::free(p); return nullptr; }
        if (!p) return mem::alloc(size, align ? align : 8, mem::Tag::Rendering);
        if (align <= 8) return mem::realloc(p, size);
        // Over-aligned grow: mem::realloc keeps provenance but not alignment,
        // so move by hand.
        void* np = mem::alloc(size, align, mem::Tag::Rendering);
        if (!np) return nullptr;
        const size_t old = mem::allocSize(p);
        std::memcpy(np, p, old < size ? old : size);
        mem::free(p);
        return np;
    }
};
BgfxMemAllocator s_bgfxAllocator;
} // namespace

#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "core/transform.h"
#include "core/transform_utils.h"        // getWorldMatrix
#include "components/mesh_renderer.h"
#include "components/light.h"

#include "render/forward_pipeline.h"     // ForwardPipeline + compiled shader bins (single TU)
#include "render/shader/shader_library.h"
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

    // ── Single-threaded bgfx, deliberately. MEASURED 2026-08-03. ────────────
    // Calling renderFrame() BEFORE init tells bgfx not to spawn a render thread;
    // bgfx::frame() then renders inline on this thread.
    //
    // The old note here said a render thread "races with platform data / a null
    // window". That does not reproduce: platformData.nwh is passed through the
    // Init struct below, before bgfx::init, which is the documented-safe order.
    // Removing this call runs 600 frames without a crash. What it DOES do is
    // worse and less obvious, over three runs of `engine_host --frames 600`:
    //
    //             mean cadence        fps            worst frame
    //   1 thread  8.36/8.33/8.33 ms   119.6/120/120  30.3/16.0/20.2 ms
    //   2 threads 10.17/8.39/10.18    98.3/119.1/98.3  1008.0/41.0/1008.4 ms
    //
    // Two of three multithreaded runs stall for ~1 SECOND. That is not bgfx's
    // API semaphore giving up (its timeout is 5000 ms); the round ~1 s and the
    // "blocked waiting for next drawable" stack from an earlier Instruments
    // capture both point at Metal drawable acquisition starving once submit and
    // render are pipelined.
    //
    // And even the clean run buys nothing: this engine's per-frame CPU work is
    // 0.38 ms against an 8.33 ms display period, so there is no submit cost to
    // overlap with rendering. Pipelining also adds a frame of latency by
    // construction (bgfx::frame waits on the render thread, which renders the
    // PREVIOUS frame) — directly against the motion-to-photon budget this engine
    // is being built for.
    //
    // REVISIT WHEN, and not before: FrameStatsChannel's `work` approaches the
    // frame period — i.e. when the main thread's own CPU work caps the frame
    // rate instead of the display. Today `present` is 95% of the frame. With a
    // render thread the numbers to watch are RenderStats' waitSubmit/waitRender,
    // which read 0 in single-threaded mode and were 3.07/7.66 ms in the
    // experiment: both threads mostly waiting, because neither is the bottleneck.
    bgfx::renderFrame();

    // A windowed platform with no window is not a state bgfx can be handed: in
    // single-threaded mode it fails later and more confusingly, and it is the one
    // half of the old comment above that described a real hazard. Fail here,
    // where the message can name the cause.
    if (!nwh) {
        std::printf("[Renderer] init called with a null native window handle — "
                    "the platform reports it supports rendering but produced no "
                    "window. Refusing to initialise bgfx.\n");
        return false;
    }

    bgfx::Init init;
    init.allocator = &s_bgfxAllocator;   // Rendering heap (see above)
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

    // Before the pipeline attaches: onAttach() asks for programs.
    m_shaderLib = std::make_unique<ShaderLibrary>();
    m_shaderLib->setSearchRoot(m_shaderCacheRoot);   // may be empty until openProject

    if (!m_pipeline) {
        auto fp = std::make_unique<ForwardPipeline>();
        fp->setShadowResolution(m_shadowResolution);   // before onAttach
        m_pipeline = std::move(fp);
    }
    RenderContext rc = makeContext();
    m_pipeline->onAttach(rc);

    m_initialized = true;
    return true;
}

void Renderer::setShadowResolution(uint32_t px) {
    if (px == m_shadowResolution) return;
    m_shadowResolution = px;
    if (!m_initialized) return;          // picked up by init()

    // The shadow map is created at attach time and never resized, so applying
    // a new size means re-attaching. Only a custom pipeline that is not the
    // default forward one is left alone — it owns its own targets and this
    // setting says nothing about them.
    if (auto* fp = dynamic_cast<ForwardPipeline*>(m_pipeline.get())) {
        fp->onDetach();
        fp->setShadowResolution(px);
        RenderContext rc = makeContext();
        fp->onAttach(rc);
    }
}

void Renderer::setShaderCacheRoot(const std::filesystem::path& cacheRoot) {
    if (!m_shaderLib) {                      // headless / pre-init
        m_shaderCacheRoot = cacheRoot;
        return;
    }
    m_shaderCacheRoot = cacheRoot;
    m_shaderLib->setSearchRoot(cacheRoot);
    if (!m_initialized || !m_pipeline) return;

    // openProject() runs after init(), so the pipeline already attached against
    // an empty cache and built the compiled-in fallback. Re-attach so the cooked
    // program actually gets used — without this the whole path silently no-ops
    // and every shader edit appears to do nothing.
    m_pipeline->onDetach();
    RenderContext rc = makeContext();
    m_pipeline->onAttach(rc);
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

void Renderer::frame() { bgfx::frame(); }

bool Renderer::homogeneousDepth() const {
    return bgfx::getCaps()->homogeneousDepth;
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
        getWorldMatrixLerp(e, m_simAlpha, it.model.m);
        it.mesh = mesh;
        MaterialHandle mh = mr.materialOverride.valid()
                            ? mr.materialOverride : mesh->material;
        it.material = mh;                    // fallback for submesh ranges
        it.mat = mh.valid() ? m_materials->getMaterial(mh) : nullptr;
        it.tex = (it.mat && it.mat->hasTexture())
                 ? m_textures->getTexture(it.mat->baseColorTexture) : nullptr;
        it.meshKey = mr.mesh.id;
        it.matKey  = mh.id;

        // Bounds are COPIED here, not read through `mesh` during culling.
        // Extraction is where a Mesh pointer is legitimately live; visibility
        // runs once per view (and shadow cascades multiply that), so it should
        // scan contiguous PODs rather than chase a pointer into a GPU-resource
        // object per item per view. It also keeps rworld:: free of bgfx, which
        // is what makes culling testable at all (tests/render_world_test.cpp).
        it.hasBounds = mesh->hasBounds();
        if (it.hasBounds) {
            it.boundsCenter = mesh->boundsCenter();
            it.boundsSize   = mesh->boundsSize();
        }

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
        float m[16]; getWorldMatrixLerp(e, m_simAlpha, m);
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
    rc.shaders = m_shaderLib.get();
    // Resolved by NAME from the cooked cache — a dist has no registry.
    if (m_shaderLib) rc.standardShader = m_shaderLib->resolveByName("standard");
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
    // After the pipeline released its programs, before bgfx goes down.
    if (m_shaderLib) { m_shaderLib->shutdown(); m_shaderLib.reset(); }
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
