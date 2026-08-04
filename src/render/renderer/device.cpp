// ── Renderer: the bgfx device ────────────────────────────────────────────────
//
// ONE concern: bringing the GPU device up and down. init(), shutdown(), and the
// two calls that only exist because the runtime must never touch bgfx itself
// (frame(), homogeneousDepth()). Nothing here knows what a RenderItem is.
//
// This is also where the engine's bgfx allocator lives, so every bgfx/bx byte is
// tagged to the Rendering heap. It is a file-static because it must outlive
// bgfx::shutdown().
#include "render/renderer.h"

#include <cstdio>
#include <cstring>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/allocator.h>

#include "core/memory/mem.h"
#include "render/forward_pipeline.h"
#include "render/shader/shader_library.h"

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

    // Partitioned on ChildOf — see the declarations in renderer.h for why.
    m_itemQuery = editorWorld.query_builder<const Transform, const MeshRenderer,
                                            const PrevTransform*>()
                      .without(flecs::ChildOf, flecs::Wildcard).build();
    m_childItemQuery = editorWorld.query_builder<const Transform, const MeshRenderer,
                                                 const PrevTransform*>()
                      .with(flecs::ChildOf, flecs::Wildcard).build();
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

void Renderer::shutdown() {
    if (m_pipeline) m_pipeline->onDetach();
    // After the pipeline released its programs, before bgfx goes down.
    if (m_shaderLib) { m_shaderLib->shutdown(); m_shaderLib.reset(); }
    destroyTargets();
    if (bgfx::isValid(m_flatNormalTex)) bgfx::destroy(m_flatNormalTex);
    if (bgfx::isValid(m_whiteTex))      bgfx::destroy(m_whiteTex);
    bgfx::shutdown();
    m_initialized = false;
}

void Renderer::resize(int w, int h) {
    m_backW = w; m_backH = h;
    bgfx::reset((uint32_t)w, (uint32_t)h, BGFX_RESET_VSYNC);
}

void Renderer::frame() { bgfx::frame(); }

bool Renderer::homogeneousDepth() const {
    return bgfx::getCaps()->homogeneousDepth;
}
