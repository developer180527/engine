// ── EngineRuntime — BOOT phase ───────────────────────────────────────────────
// One of runtime's four phase TUs (boot / frame / sim / lifecycle core in
// runtime.cpp). This one stands the engine up: platform + renderer + systems
// + optional default scene. Nothing here runs after init() returns.
// NO <bgfx/bgfx.h> in any runtime TU — GPU work goes through Renderer /
// PrimitiveLibrary (audit A.1).
#include "runtime/runtime.h"

// The IRenderer implementations. This is the ONLY runtime TU that names either —
// runtime.h holds a std::unique_ptr<IRenderer> and nothing else in the runtime
// knows a concrete renderer exists.
//
// ENGINE_SERVER_BUILD (G1c step B) drops the real one. It is the single #if the
// server target needs, and it is here because this file holds the FACTORY: a
// build-time choice belongs at the point where the choice is made, not sprinkled
// through the call sites. Everything downstream still calls IRenderer and cannot
// tell which build it is in.
#if !ENGINE_SERVER_BUILD
#include "render/renderer.h"
#endif
#include "render/renderer_null.h"
#include "runtime/platform/glfw_platform.h"
#include "core/logger.h"
#include "runtime/scripting/script_host.h"
#include "runtime/services/anim_service.h"
#include "runtime/scripting/script_services.h"
#include "runtime/scripting/engine_api_binding.h"
#include "runtime/mem_channel.h"
#include "runtime/frame_stats_channel.h"
#include "runtime/jobs/jobs.h"
#include "core/memory/mem.h"
#include <ozz/base/memory/allocator.h>

#include <cstdio>
#include <utility>

// Shipping builds (ENGINE_WITH_SOURCE_IMPORTERS=0) compile the source-format
// import stack out entirely: cooked binaries are the ONLY content path and
// Assimp is never linked. Dev trees keep drag-drop import.
#if ENGINE_WITH_SOURCE_IMPORTERS
#include "assets/importers/gltf_importer.h"
#include "assets/importers/assimp_importer.h"
#endif
#include "animation/clip_library.h"
#include "components/meta_registry.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "core/transform.h"

namespace {
// ozz allocations route to the Animation tag; installed in init() (ozz only
// allocates at import time). The instance deliberately outlives main —
// statics may free through it.
struct OzzMemAllocator final : ozz::memory::Allocator {
    void* Allocate(size_t size, size_t align) override {
        return mem::alloc(size, align, mem::Tag::Animation);
    }
    void Deallocate(void* p) override { mem::free(p); }
};
OzzMemAllocator g_ozzAllocator;
} // namespace

bool EngineRuntime::init(const EngineConfig& cfg) {
    // Whichever backend this build selected (ENGINE_WINDOW_BACKEND).
    return init(cfg, makeDefaultPlatform());
}

bool EngineRuntime::init(const EngineConfig& cfg,
                         std::unique_ptr<IPlatform> platform) {
    if (m_initialized) {
        LOG_ERROR("Runtime", "init() called twice — ignoring");
        return false;
    }
    // Profiler comes up first so it can time the boot sequence itself.
    // The memory channel registers before the boot frame so boot allocations
    // are measured too (the framework's first extra channel).
    prof::Profiler::get().setEnabled(cfg.enableProfiler);
    m_memChannel = std::make_unique<MemoryChannel>();
    prof::Profiler::get().addChannel(m_memChannel.get());
    m_inputLatency = std::make_unique<InputLatencyChannel>(&m_input);
    prof::Profiler::get().addChannel(m_inputLatency.get());
    m_frameStats = std::make_unique<FrameStatsChannel>();
    // Which log categories a GAME builder is answerable for, versus the engine
    // machinery. Warnings and errors reach the game console from everywhere
    // regardless, so this only routes info-level chatter — see logger.h.
    // Called first so nothing logged during boot lands in the wrong console.
    elog::markGameFacingDefaults();
    prof::Profiler::get().addChannel(m_frameStats.get());
    prof::Profiler::get().beginFrame();   // boot = "frame 0"
    m_frameArena.init(4 * 1024 * 1024);   // 4 MB per-frame transient pool
    // Worker pool next — spawned exactly once for the engine's lifetime;
    // animation, physics (Jolt adapter) and future systems all schedule here.
    jobs::init();
    // Memory manager warmup + remaining third-party hook (flecs hooked at
    // member-init before m_ecs; Jolt/Lua/bgfx/miniaudio hook at their own inits).
    ozz::memory::SetDefaulAllocator(&g_ozzAllocator);   // [sic] ozz API typo
    mem::init();
    m_width = cfg.width; m_height = cfg.height; m_fov = cfg.fov;

    // Project loads first so the window title can default to its name.
    // Resolve the project up front so the window title can use its name.
    // With autoDetectProject=false and no explicit root, the engine boots
    // projectless — the app opens one later via openProject() (editor hub).
    if (!cfg.projectRoot.empty())
        m_project = ProjectContext::load(cfg.projectRoot);
    else if (cfg.autoDetectProject)
        m_project = ProjectContext::autoDetect();
    // (openProject below does the logging + asset DB once systems exist)

    const std::string title = !cfg.title.empty() ? cfg.title
        : (hasProject() ? m_project.name : std::string("Engine"));
    m_platform = std::move(platform);
    { ENGINE_PROFILE_SCOPE("boot.platform");
      if (!m_platform->init({title, cfg.width, cfg.height,
                            cfg.hideTitleBar})) return false; }
    { ENGINE_PROFILE_SCOPE("boot.renderer");
      if (!initRenderer(cfg))  return false; }
    { ENGINE_PROFILE_SCOPE("boot.systems");
      if (!initSystems(cfg))   return false; }
    if (hasProject()) { ENGINE_PROFILE_SCOPE("boot.openProject");
      openProject(m_project.projectRoot); } // DB + service wiring
    if (cfg.defaultScene) { ENGINE_PROFILE_SCOPE("boot.defaultScene");
      buildDefaultScene(); }
    m_initialized = true;

    // Dump the boot breakdown (boot = the profiler frame opened above).
    prof::Profiler::get().endFrame();
    if (cfg.enableProfiler) prof::Profiler::get().timer().logLastFrame("Boot");
    // Boot was "frame 0", so without this the first real frame's cadence would
    // be measured from the start of init — one multi-second sample that skews
    // every tail percentile in the report.
    if (m_frameStats) m_frameStats->reset();
    return true;
}

bool EngineRuntime::initRenderer(const EngineConfig& cfg) {
    // THE ONE PLACE that decides which renderer exists. Everything downstream
    // calls through IRenderer and never asks again — which is what removed the
    // scattered `if (!m_headless)` guards around render calls, and with them the
    // class of bug where one guard is present and an adjacent one is not
    // (docs/rhi/headless.md §4).
    if (!m_platform->supportsRendering()) {
        // Headless platform — no GPU. ECS, assets, animation, physics and
        // scripting still run; render entry points do nothing, correctly.
        m_headless = true;
        m_renderer = std::make_unique<NullRenderer>();
        return true;
    }
#if ENGINE_SERVER_BUILD
    // A server build has no Renderer to construct. Reaching here means the
    // platform claimed it can render, which for a server is a configuration
    // error rather than a state to recover from — say so and stay headless
    // instead of pretending a device exists.
    LOG_WARN("Renderer", "server build: platform reports rendering support, but "
                         "this binary contains no renderer — staying headless");
    m_headless = true;
    m_renderer = std::make_unique<NullRenderer>();
    return true;
#else
    m_renderer = std::make_unique<Renderer>();

    void* nwh = m_platform->nativeWindowHandle();
    // Graphics quality from project data, applied BEFORE init so the shadow
    // map is created at the right size the first time (a later openProject
    // re-applies it). This is the engine's largest single GPU allocation.
    m_renderer->setShadowResolution(m_project.graphics.shadowResolution);
    return m_renderer->init(nwh, cfg.width, cfg.height,
                           m_ecs, m_assets, m_textures, m_materials,
                           m_skeletons);
#endif
}

bool EngineRuntime::initSystems(const EngineConfig& cfg) {
    m_openAssetDatabase = cfg.openAssetDatabase;
    // Reflection backbone: engine component schemas registered in the
    // runtime's world (the game world in Snapshot mode registers in
    // startSimulation). Kits register their own at attach/sim-start.
    MetaRegistry::registerAll(m_ecs);
    m_clipLibrary = std::make_unique<ClipLibrary>();
    // Source-format importers exist to produce GPU meshes — headless runs
    // (dedicated server, CI sim) have no device to feed, so don't stand up
    // the offline import stack (Assimp is a large, editor-facing parsing
    // library; audit A.3 — it was registered unconditionally for every
    // runtime instance including shipped games' headless paths). Shipping
    // builds compile them out entirely (ENGINE_WITH_SOURCE_IMPORTERS=0).
// && !ENGINE_SERVER_BUILD: a server loads COOKED BINARIES ONLY. The importer
// TUs are not in that target (they pull Assimp and the glTF parser), so this
// registration would be an undefined vtable at link time — and it should be:
// parsing FBX on a dedicated server is not a feature anyone wants back.
#if ENGINE_WITH_SOURCE_IMPORTERS && !ENGINE_SERVER_BUILD
    if (!m_headless) {
        m_importers.registerImporter(std::make_unique<GltfImporter>());
        m_importers.registerImporter(std::make_unique<AssimpImporter>());
    }
#endif

    // AssetService — async mesh/texture loading for scripts + scene streaming.
    // Skeleton/clip registries wired so SKINNED cooked meshes stream too
    // (v3 payload: bones + ozz archives — no source parse, ship-safe).
    m_assetService = std::make_unique<AssetService>(AssetService::Config{
        m_assets, m_textures, m_materials,
        /*assetLib*/ nullptr, /*projectRoot*/ {},
        &m_skeletons, &m_clips});
    if (cfg.meshBudgetMB > 0)
        m_assetService->setResidencyBudget(
            (uint64_t)cfg.meshBudgetMB * 1024 * 1024);
    if (cfg.textureBudgetMB > 0)
        m_assetService->setTextureBudget(
            (uint64_t)cfg.textureBudgetMB * 1024 * 1024);

    // Per-tag CPU ceilings. Soft: the allocator warns once per tag and keeps
    // going (see EngineConfig::memBudgetMB). Applied here rather than in
    // mem::init() because they are PROJECT policy, and mem:: has no idea a
    // project exists — it is the layer everything else allocates through.
    for (size_t t = 0; t < (size_t)mem::Tag::Count; ++t)
        if (cfg.memBudgetMB[t] > 0)
            mem::setBudget((mem::Tag)t,
                           (uint64_t)cfg.memBudgetMB[t] * 1024 * 1024);

    // SceneService — built on top of AssetService for binary scene loading.
    // ClipLibrary + registries enable skinned spawn wiring (SkinnedMesh +
    // Animator with clip binding) for cooked scenes.
    m_sceneService = std::make_unique<SceneService>(SceneService::Config{
        *m_assetService, m_assets, m_textures, m_materials, m_ecs,
        &m_primitives, m_clipLibrary.get(), &m_skeletons, &m_clips});

    // ScriptHost — the canonical scripting surface. Lua, the C API
    // (engine_api.h, used by hot-reloaded game modules) and future language
    // hosts all drive this one object; the world binds at startSimulation.
    m_scriptHost = std::make_unique<ScriptHost>(nullptr);
    m_scriptHost->setAssetService(m_assetService.get());
    m_scriptHost->setSceneService(m_sceneService.get());
    m_scriptHost->setPlatform(m_platform.get());   // cursor capture via C API
    m_scriptHost->setDebugDraw(&m_debugDraw);       // engineDraw* -> collector
    m_animService = std::make_unique<AnimService>();
    m_animService->init(&m_skeletons, &m_clips, m_clipLibrary.get(), &m_project);
    m_scriptHost->setAnimService(m_animService.get());
    m_scriptHost->setNavService(&m_nav);   // engine navmesh over the nav C-API

    // Input: raw hid source when available (Input Monitoring on macOS),
    // window fallback otherwise. Bindings from the project's input.json.
    m_input.init(m_project.projectRoot);
    engineInputBindManager(&m_input);
    m_renderer->setDebugDraw(&m_debugDraw);          // collector -> line pass
    engineApiBindHost(m_scriptHost.get());
    // The primitive tier: jobs and memory bind to process-wide facades and need
    // nothing here, but frameAlloc and draw submission are runtime-owned.
    engineMemBindFrameArena(&m_frameArena);
    engineDrawSubmitBindRenderer(m_renderer.get());

    m_ctx = std::make_unique<RuntimeContext>(RuntimeContext{
        m_ecs, m_assets, m_textures,
        m_materials, m_project, m_importers});
    // Primitive meshes are GPU buffers — skipped headless. This guard is a
    // WORK-SKIP, not a safety guard, and the distinction changed in G1a: it used
    // to say "touching bgfx null-derefs the allocator", which is no longer true
    // (gpu::copy returns null with no device, render/gpu.h). Calling this
    // headless would now be harmless — it would just build nothing, slowly. It
    // stays because servers and CLI tools have no use for a cube.
    if (!m_headless) m_primitives.init(m_assets);
    m_ctx->assetLib      = &m_assetLib;
    m_ctx->primitives    = &m_primitives;
    m_ctx->assetService  = m_assetService.get();
    m_ctx->sceneService  = m_sceneService.get();
    m_ctx->skeletons     = &m_skeletons;
    m_ctx->clips         = &m_clips;
    m_ctx->clipLibrary   = m_clipLibrary.get();
    m_ctx->scriptHost    = m_scriptHost.get();

    // (Spinner query is a lazy WorldQueryCache on simWorld() — see tickSystems.)

    // Animation system — samples clips and writes bone palettes each frame.
    m_animatorSystem.init(m_ecs, m_skeletons, m_clips);

    return true;
}

void EngineRuntime::buildDefaultScene() {
    if (m_headless) return; // GPU meshes — nothing to build without a device

    // The demo grid reuses PrimitiveLibrary's cube (built in initSystems) —
    // this function used to duplicate the cube's GPU-buffer construction
    // with raw bgfx calls, one of runtime.cpp's three direct-bgfx sites.
    if (!m_primitives.ready()) return;
    MeshHandle cubeHandle = m_primitives.cube();

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
