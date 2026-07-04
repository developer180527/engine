#include "runtime/runtime.h"
#include "runtime/platform/glfw_platform.h"
#include "core/logger.h"
#include "runtime/scripting/script_host.h"
#include "runtime/services/anim_service.h"
#include "runtime/scripting/script_services.h"
#include "runtime/scripting/engine_api_binding.h"
#include "runtime/mem_channel.h"
#include "runtime/jobs/jobs.h"
#include "runtime/input/input_system.h"
#include "core/memory/mem.h"
#include <ozz/base/memory/allocator.h>

// ── Third-party allocators → tagged heaps ───────────────────────────────────
// flecs: hooked at STATIC INIT because EngineRuntime's member world is
// constructed with the object itself — before init() could run. The default
// impls also maintain flecs's own counters (MemoryChannel reads them), so the
// replacements keep them alive. ozz: installed in init() (its allocations
// only start at import time); the instance leaks by design (statics may free
// through it after main).
namespace {

bool hookFlecsAllocator() {
    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_api;
    api.malloc_ = [](ecs_size_t size) -> void* {
        ecs_os_api_malloc_count++;
        return mem::alloc((size_t)size, 16, mem::Tag::ECS);
    };
    api.calloc_ = [](ecs_size_t size) -> void* {
        ecs_os_api_calloc_count++;
        void* p = mem::alloc((size_t)size, 16, mem::Tag::ECS);
        if (p) std::memset(p, 0, (size_t)size);
        return p;
    };
    api.realloc_ = [](void* p, ecs_size_t size) -> void* {
        ecs_os_api_realloc_count++;
        if (!p) return mem::alloc((size_t)size, 16, mem::Tag::ECS);
        return mem::realloc(p, (size_t)size);
    };
    api.free_ = [](void* p) {
        ecs_os_api_free_count++;
        mem::free(p);
    };
    ecs_os_set_api(&api);
    return true;
}
[[maybe_unused]] const bool g_flecsHooked = hookFlecsAllocator();

struct OzzMemAllocator final : ozz::memory::Allocator {
    void* Allocate(size_t size, size_t align) override {
        return mem::alloc(size, align, mem::Tag::Animation);
    }
    void Deallocate(void* p) override { mem::free(p); }
};
OzzMemAllocator g_ozzAllocator;

} // namespace

#include <cstdio>
#include <utility>

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include "render/vertex.h"
#include "render/primitive_cube.h"
#include "render/mesh.h"
#include "runtime/camera_util.h"
#include "assets/importers/gltf_importer.h"
#include "assets/importers/assimp_importer.h"
#include "assets/asset_storage.h"
#include "scene/scene_serializer.h"
#include "scene/reflected_serde.h"
#include "animation/clip_library.h"
#include "components/meta_registry.h"
#include "components/prev_transform.h"
#include "components/camera.h"
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
    prof::Profiler::get().beginFrame();   // boot = "frame 0"
    m_frameArena.init(4 * 1024 * 1024);   // 4 MB per-frame transient pool
    // Worker pool next — spawned exactly once for the engine's lifetime;
    // animation, physics (Jolt adapter) and future systems all schedule here.
    jobs::init();
    // Memory manager warmup + remaining third-party hook (flecs hooked at
    // static init above; Jolt/Lua/bgfx/miniaudio hook at their own inits).
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
      if (!m_platform->init({title, cfg.width, cfg.height})) return false; }
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
    return true;
}

void EngineRuntime::attachPlugins() {
    if (!m_initialized) {
        LOG_ERROR("Runtime", "attachPlugins() before init()");
        return;
    }
    if (m_pluginsAttached) {
        LOG_WARN("Runtime", "attachPlugins() called twice — ignoring "
                 "(plugins added later need manual onAttach)");
        return;
    }
    m_pluginsAttached = true;
    m_plugins.attachAll(*m_ctx);
}

// Out-of-line: ClipLibrary is forward-declared in the header (it pulls Assimp,
// which the kit-facing header surface must not).
ClipLibrary& EngineRuntime::clipLibrary() { return *m_clipLibrary; }

bool EngineRuntime::openProject(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (!m_initialized) {
        // init() calls this internally AFTER systems exist but BEFORE the
        // flag flips — allow that path; reject true pre-init external calls.
        if (!m_assetService) {
            LOG_ERROR("Project", "openProject() before init()");
            return false;
        }
    }
    if (!fs::exists(root / "project.json")) {
        LOG_WARN("Project", "No project.json at: %s", root.string().c_str());
        return false;
    }
    m_project = ProjectContext::load(root);
    LOG_INFO("Project", "Opened: %s", m_project.projectRoot.string().c_str());
    // The editor boots projectless — input bindings arrive WITH the project.
    m_input.loadProjectBindings(m_project.projectRoot);
    if (m_clipLibrary)   // cooked-clip cache lives with the project
        m_clipLibrary->setCacheRoot(m_project.projectRoot / ".cache" / "anim");

    // Asset database — open + scan so handles resolve immediately.
    const auto cacheRoot = m_project.projectRoot / ".cache";
    if (m_openAssetDatabase) {
        const auto dbPath = cacheRoot / "registry.db";
        fs::create_directories(cacheRoot);
        if (m_assetLib.open(dbPath)) {
            int n = 0;
            if (fs::exists(m_project.assetsRoot))
                n = m_assetLib.scan(m_project.assetsRoot, m_project.projectRoot);
            LOG_INFO("AssetLib", "Registry ready — %zu asset(s), %d new/updated",
                     m_assetLib.all().size(), n);
        } else {
            LOG_WARN("AssetLib", "Could not open registry at: %s",
                     dbPath.string().c_str());
        }
    }

    // Point the content services at the project.
    if (m_assetService) {
        m_assetService->setAssetLib(&m_assetLib);
        m_assetService->setProjectRoot(m_project.projectRoot);
    }
    if (m_sceneService) m_sceneService->setCacheRoot(cacheRoot);
    if (m_platform)     m_platform->setTitle(m_project.name);
    return true;
}

bool EngineRuntime::initRenderer(const EngineConfig& cfg) {
    if (!m_platform->supportsRendering()) {
        // Headless platform — no GPU. ECS, assets, animation, physics and
        // scripting still run; render entry points become no-ops.
        m_headless = true;
        return true;
    }
    void* nwh = m_platform->nativeWindowHandle();
    return m_renderer.init(nwh, cfg.width, cfg.height,
                           m_ecs, m_assets, m_textures, m_materials,
                           m_skeletons);
}

bool EngineRuntime::initSystems(const EngineConfig& cfg) {
    m_openAssetDatabase = cfg.openAssetDatabase;
    // Reflection backbone: engine component schemas registered in the
    // runtime's world (the game world in Snapshot mode registers in
    // startSimulation). Kits register their own at attach/sim-start.
    MetaRegistry::registerAll(m_ecs);
    m_clipLibrary = std::make_unique<ClipLibrary>();
    m_importers.registerImporter(std::make_unique<GltfImporter>());
    m_importers.registerImporter(std::make_unique<AssimpImporter>());

    // AssetService — async mesh/texture loading for scripts + scene streaming.
    m_assetService = std::make_unique<AssetService>(AssetService::Config{
        m_assets, m_textures, m_materials});

    // SceneService — built on top of AssetService for binary scene loading.
    m_sceneService = std::make_unique<SceneService>(SceneService::Config{
        *m_assetService, m_assets, m_textures, m_materials, m_ecs, &m_primitives});

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

    // Input: raw hid source when available (Input Monitoring on macOS),
    // window fallback otherwise. Bindings from the project's input.json.
    m_input.init(m_project.projectRoot);
    engineInputBindManager(&m_input);
    m_renderer.setDebugDraw(&m_debugDraw);          // collector -> line pass
    engineApiBindHost(m_scriptHost.get());

    m_ctx = std::make_unique<RuntimeContext>(RuntimeContext{
        m_ecs, m_assets, m_textures,
        m_materials, m_project, m_importers});
    // Primitive meshes are GPU buffers — skip them headless (bgfx is never
    // initialized without a render device; touching it null-derefs the
    // allocator). Servers/CLI tools run fine without them.
    if (!m_headless) m_primitives.init(m_assets);
    m_ctx->assetLib      = &m_assetLib;
    m_ctx->primitives    = &m_primitives;
    m_ctx->assetService  = m_assetService.get();
    m_ctx->sceneService  = m_sceneService.get();
    m_ctx->skeletons     = &m_skeletons;
    m_ctx->clips         = &m_clips;
    m_ctx->clipLibrary   = m_clipLibrary.get();
    m_ctx->scriptHost    = m_scriptHost.get();

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
    if (!m_initialized) {
        LOG_ERROR("Runtime", "frameBegin()/run() before init()");
        return false;
    }
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

    prof::Profiler::get().beginFrame();
    m_frameArena.reset();   // last frame's transient allocations are freed here
    m_debugDraw.clear();    // debug lines are per-frame — kits re-queue each frame

    // Drain async asset uploads (main-thread GPU upload)
    { ENGINE_PROFILE_SCOPE("AsyncDrain");
      if (m_assetService) m_assetService->drainUploads(); }

    return true;
}

void EngineRuntime::frameEnd() {
    { ENGINE_PROFILE_SCOPE("bgfx.frame");
      if (!m_headless) bgfx::frame(); }
    prof::Profiler::get().endFrame();
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

bool EngineRuntime::tick(float dt) {
    tickSystems(dt, false);
    tickSimulation(dt);
    if (m_headless) return false;

    float view[16], proj[16], clear[4];
    const float aspect = m_height > 0
        ? float(m_width) / float(m_height) : 16.0f / 9.0f;
    if (!m_cameraFinder.find(simWorld(), view, proj, aspect, clear))
        return false;

    flecs::world* world = m_gameWorld ? m_gameWorld.get() : nullptr;
    ENGINE_PROFILE_SCOPE("Render");
    m_renderer.renderToBackbuffer(view, proj, clear, world);
    return true;
}

bool EngineRuntime::startSimulation(SimMode mode) {
    if (!m_initialized) {
        LOG_ERROR("Sim", "startSimulation() before init()");
        return false;
    }
    if (m_simulating) return true;

    if (mode == SimMode::Snapshot) {
        AssetStorage storage{m_assets, m_textures, m_materials,
                             &m_skeletons, &m_clips};
        m_simSnapshot = SceneSerializer::saveToString(m_ecs, storage);
        m_gameWorld   = std::make_unique<flecs::world>();
        // Fresh world: engine schemas must exist before the snapshot populates
        // it, or engine reflected components would all land in Pending.
        MetaRegistry::registerAll(*m_gameWorld);
        SceneSerializer::loadIntoWorld(m_simSnapshot, *m_gameWorld, storage);
    }

    m_simulating = true;
    m_simElapsed = 0.0;
    m_simFrame   = 0;
    // Bind the script surface to the sim world BEFORE plugins start — Lua
    // instantiates script instances during broadcastSimStart.
    m_scriptHost->setWorld(&simWorld());
    m_scriptHost->beginSession();   // invalidate entity refs from prior runs
    // Lazily dlopen the project's kits and attach them — they join the registry
    // BEFORE the broadcast so their onSimulationStart fires with everyone else.
    m_kits.start(m_project, m_plugins, *m_ctx);
    m_plugins.broadcastSimStart(simWorld());
    // Kits have now registered their component schemas (attach/sim-start) —
    // apply scene data that was waiting for those types. The editor world gets
    // a pass too: kit onAttach registers against it, so after the first Play
    // the editor can author/inspect kit components directly.
    reflected::applyPending(simWorld());
    if (m_gameWorld) reflected::applyPending(m_ecs);
    LOG_SUCCESS("Sim", "Simulation started (%s)",
                mode == SimMode::Snapshot ? "snapshot" : "in-place");
    return true;
}

void EngineRuntime::stopSimulation() {
    if (!m_simulating) return;
    m_plugins.broadcastSimStop();
    // Kits detach + dlclose AFTER the stop broadcast (so onSimulationStop has
    // fired on them) and before the sim world is torn down below.
    m_kits.stop(m_plugins);
    m_scriptHost->setWorld(nullptr);            // C API + Lua go dormant
    // The snapshot world dies below — drop every cached query against it
    // (a future world could reuse the same address and false-match).
    m_cameraFinder.reset();
    m_animatorSystem.resetWorldCache();
    m_renderer.resetWorldCaches();
    m_scriptHost->setPhysicsService(nullptr);
    m_scriptHost->setAudioService(nullptr);
    m_gameWorld.reset();
    m_simSnapshot.clear();
    m_simulating = false;
    LOG_SUCCESS("Sim", "Simulation stopped");
}

void EngineRuntime::tickSimulation(float dt) {
    if (!m_simulating) return;
    flecs::world& w = simWorld();

    // Hot-reload any kit whose .so changed on disk — before the broadcasts, so
    // the registry is stable while they iterate.
    m_kits.poll(dt, m_plugins, *m_ctx, w);

    // Script-facing frame state + late service discovery (physics/audio
    // publish refs into the sim world during their onSimulationStart).
    if (const PhysicsServiceRef* pr = w.try_get<PhysicsServiceRef>())
        m_scriptHost->setPhysicsService(pr->svc);
    if (const AudioServiceRef* ar = w.try_get<AudioServiceRef>())
        m_scriptHost->setAudioService(ar->svc);

    // ── FIXED-TIMESTEP simulation ───────────────────────────────────────────
    // Gameplay/scripts/physics step at a constant kSimDt regardless of frame
    // rate: stable integration, deterministic sim, and tick-aligned
    // InputSnapshots (the netcode contract). Accumulator pattern; clamped so
    // a hitch can't death-spiral into ever more catch-up steps. Rendering
    // still runs per frame off the latest state (interpolation between the
    // last two sim states is the planned follow-up); camera look stays fresh
    // via the late-latch channel.
    m_simAccumulator += dt;
    if (m_simAccumulator > 4.0f * kSimDt) m_simAccumulator = 4.0f * kSimDt;
    while (m_simAccumulator >= kSimDt) {
        m_simAccumulator -= kSimDt;
        // Interpolation snapshot: remember where everything WAS before this
        // step so rendering can lerp. Cameras are excluded — their rotation
        // is late-latched at render rate (onFrame) and must not lag.
        w.defer_begin();   // set<> during iteration = structural op
        w.each([](flecs::entity e, Transform& t) {
            if (e.has<Camera>()) return;
            e.set<PrevTransform>({t.position, t.rotation, t.scale});
        });
        w.defer_end();
        m_input.beginTick(hid::nowNs());   // fold staged events -> snapshot
        m_simElapsed += kSimDt;
        ++m_simFrame;
        m_scriptHost->setFrame(kSimDt, m_simElapsed, m_simFrame);
        // Explicit phase order so script intent lands in the SAME physics
        // step: scripts set intent -> physics applies it -> contacts dispatch.
        { ENGINE_PROFILE_SCOPE("Sim.update");  m_plugins.broadcastUpdate(w, kSimDt); }
        { ENGINE_PROFILE_SCOPE("Sim.physics"); m_plugins.broadcastPhysicsStep(w, kSimDt); }
        { ENGINE_PROFILE_SCOPE("Sim.post");    m_plugins.broadcastPostPhysics(w); }
    }

    m_renderer.setSimAlpha(m_simAccumulator / kSimDt);   // leftover fraction

    // Render-rate hook: presentation work (late-latched camera) runs once
    // per FRAME with real dt, after the fixed steps — this is what keeps
    // look latency at frame rate even when sim ticks slower.
    { ENGINE_PROFILE_SCOPE("Sim.frame"); m_plugins.broadcastFrame(w, dt); }

    // Snapshot mode runs in a separate world that tickSystems never touches —
    // run animation and the flecs pipeline here so play mode behaves exactly
    // like in-place simulation. (In-place mode: simWorld() == m_ecs, which
    // tickSystems already animates and progresses — don't double-tick.)
    if (m_gameWorld) {
        m_animatorSystem.tick(*m_gameWorld, dt);
        m_gameWorld->progress(dt);
    }
}

void EngineRuntime::tickSystems(float dt, bool paused) {
    if (!m_initialized) { LOG_ERROR("Runtime", "tick() before init()"); return; }
    jobs::pumpMain();   // drain job->main-thread requests; sweep finished jobs
    // Input: drain sources, mirror UI/focus gates, fold a tick snapshot.
    // (Per-frame tick today; slots into the fixed-timestep loop when it lands.)
    m_input.setFocused(InputSystem::get().windowFocused());
    m_input.setUICapture(InputSystem::get().uiCapturesKeyboard(),
                         InputSystem::get().uiCapturesMouse());
    m_input.pump();
    if (!m_simulating) m_input.beginTick(hid::nowNs());
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
    { ENGINE_PROFILE_SCOPE("Animation");   m_animatorSystem.tick(dt); }
    { ENGINE_PROFILE_SCOPE("ECS.progress"); m_ecs.progress(); }
}

void EngineRuntime::shutdown() {
    if (!m_initialized) return;   // idempotent / never-initialized safe
    m_initialized = false;
    if (m_memChannel) { prof::Profiler::get().removeChannel(m_memChannel.get());
                        m_memChannel.reset(); }
    if (m_inputLatency) { prof::Profiler::get().removeChannel(m_inputLatency.get());
                          m_inputLatency.reset(); }
    stopSimulation();      // broadcasts onSimulationStop if still running
    engineApiBindHost(nullptr);
    engineInputBindManager(nullptr);
    m_input.shutdown();
    m_plugins.detachAll(); // plugins may hold services — detach before teardown
    jobs::shutdown();      // after plugins: Jolt's adapter schedules here
    mem::shutdown();       // report-only: final per-tag residency dump
    m_clips.clear();
    m_skeletons.clear();
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    if (!m_headless) m_renderer.shutdown();
    if (m_platform) { m_platform->shutdown(); m_platform.reset(); }
}
