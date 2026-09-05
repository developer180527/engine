// ── EngineRuntime — LIFECYCLE CORE ───────────────────────────────────────────
// EngineRuntime is an ORCHESTRATOR: it owns the subsystems and sequences
// them; it implements as little as possible itself. The implementation is
// split into phase TUs so no single file accretes into a god object:
//   runtime.cpp        — this file: construction, allocator hooks, project
//                        open, plugin attach, shutdown
//   runtime_boot.cpp   — init(): platform + renderer + systems + default scene
//   runtime_frame.cpp  — frameBegin/frameEnd/run/resize + tick entry points
//   runtime_sim.cpp    — play sessions, fixed-timestep loop, system tick
// NO <bgfx/bgfx.h> in any of them — GPU work goes exclusively through
// Renderer / PrimitiveLibrary (audit A.1).
#include "runtime/runtime.h"

#include "render/renderer_null.h"   // the ctor's default renderer
#include "core/logger.h"
#include "runtime/scripting/script_host.h"
#include "runtime/services/anim_service.h"
#include "runtime/scripting/engine_api_binding.h"
#include "runtime/mem_channel.h"
#include "runtime/frame_stats_channel.h"
#include "runtime/jobs/jobs.h"
#include "runtime/input/input_system.h"
#include "core/memory/mem.h"

#include <cstring>
#include <filesystem>

#include "animation/clip_library.h"

// ── flecs allocator → tagged heaps ──────────────────────────────────────────
// Hooked BEFORE any world exists: EngineRuntime's member-init list calls
// engine_detail::ensureFlecsAllocatorHooked() immediately before m_ecs
// constructs (see runtime.h). The default impls also maintain flecs's own
// counters (MemoryChannel reads them), so the replacements keep them alive.
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

} // namespace

namespace engine_detail {
// Sequenced from EngineRuntime's member-init list, immediately before m_ecs
// constructs — declaration order guarantees the hook precedes the first
// world. The old TU-global initializer only ordered correctly because
// EngineRuntime happened to be built inside main(); a static-duration
// flecs::world in any other TU would have raced it (audit M.6). Idempotent.
bool ensureFlecsAllocatorHooked() {
    static const bool hooked = hookFlecsAllocator();
    return hooked;
}
} // namespace engine_detail

// A NullRenderer from CONSTRUCTION, not from initRenderer(). The public API —
// resize(), createSceneFB(), renderGameView(), sceneW/H(), renderer() — all
// dereference m_renderer with no init guard, and the old `Renderer m_renderer`
// by value was safe to call at any point in the lifecycle. A unique_ptr is not.
//
// Our own boot path never exposed it (a failed platform init returns before
// initRenderer, and shutdown() early-returns on !m_initialized), but an EMBEDDER
// does: platform-embedder.md gives the host the window and the event loop, so a
// host resize callback arriving around a failed or in-progress init is exactly
// this hazard. initRenderer() replaces this; until then it is a working renderer
// that draws nothing, which makes "never null" a property of the type rather
// than an argument about ordering.
EngineRuntime::EngineRuntime()
    : m_renderer(std::make_unique<NullRenderer>()) {}
EngineRuntime::~EngineRuntime() = default;

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
    // Cooked shaders resolve by NAME from .cache/shaders — no registry lookup,
    // because a shipped dist has none (engine_player sets
    // openAssetDatabase=false and resolves everything by baked paths).
    m_renderer->setShaderCacheRoot(cacheRoot);

    if (m_sceneService) m_sceneService->setCacheRoot(cacheRoot);
    if (m_platform)     m_platform->setTitle(m_project.name);
    return true;
}

void EngineRuntime::shutdown() {
    if (!m_initialized) return;   // idempotent / never-initialized safe
    m_initialized = false;
    if (m_memChannel) { prof::Profiler::get().removeChannel(m_memChannel.get());
                        m_memChannel.reset(); }
    if (m_inputLatency) { prof::Profiler::get().removeChannel(m_inputLatency.get());
                          m_inputLatency.reset(); }
    // Frame-time distribution, printed once at exit: every windowed app gets
    // the report without its own code, and it is the only place present/vsync
    // cost is real. Report BEFORE unregistering.
    if (m_frameStats) {
        if (prof::Profiler::get().enabled() && m_frameStats->totalFrames() > 0)
            m_frameStats->logDistribution("shutdown");
        prof::Profiler::get().removeChannel(m_frameStats.get());
        m_frameStats.reset();
    }
    stopSimulation();      // broadcasts onSimulationStop if still running
    engineApiBindHost(nullptr);
    engineInputBindManager(nullptr);
    // Unbind before the owners die: a kit ticking during shutdown would
    // otherwise submit into a destroyed renderer or a freed arena.
    engineMemBindFrameArena(nullptr);
    engineDrawSubmitBindRenderer(nullptr);
    m_input.shutdown();
    // Same reason as in stopSimulation(): a queued main-thread callback may live
    // in a plugin about to be detached (and, for a kit, unloaded). Run it while
    // its code is still mapped — jobs::shutdown() below would otherwise discard
    // the queue silently, and any pump in between would call into freed code.
    jobs::drainMain();
    m_plugins.detachAll(); // plugins may hold services — detach before teardown
    jobs::shutdown();      // after plugins: Jolt's adapter schedules here
    mem::shutdown();       // report-only: final per-tag residency dump
    m_clips.clear();
    m_skeletons.clear();
    m_materials.clear();
    m_textures.clear();
    m_assets.clear();
    m_renderer->shutdown();
    if (m_platform) { m_platform->shutdown(); m_platform.reset(); }
}
