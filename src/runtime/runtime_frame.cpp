// ── EngineRuntime — FRAME phase ──────────────────────────────────────────────
// One of runtime's four phase TUs (boot / frame / sim / lifecycle core in
// runtime.cpp). Per-frame orchestration: begin/end, the host frame loop,
// resize, and the two tick entry points (editor-camera and game-camera).
// NO <bgfx/bgfx.h> — frame flip and caps queries go through Renderer.
#include "runtime/runtime.h"
#include "core/logger.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"
#include "components/mesh_renderer.h"

#include <chrono>
#include <unordered_set>

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

    // Residency sweep (audit Q6: the loaded-mesh cache grew without bound):
    // once per ~second, evict least-recently-used cooked meshes over the
    // configured budget — but never one a live MeshRenderer still points
    // at, in EITHER world (yanking a mesh under the renderer = broken
    // draws). No budget configured (editor default) = no sweep.
    if (m_assetService && ++m_residencyTick >= 60) {
        m_residencyTick = 0;
        ENGINE_PROFILE_SCOPE("AssetResidency");
        std::unordered_set<uint32_t> used;
        auto collect = [&](flecs::world& w) {
            w.each([&](flecs::entity, const MeshRenderer& mr) {
                if (mr.mesh.valid()) used.insert(mr.mesh.id);
            });
        };
        collect(m_ecs);
        if (m_gameWorld) collect(*m_gameWorld);
        m_assetService->evictOverBudget(used);
    }

    return true;
}

void EngineRuntime::frameEnd() {
    { ENGINE_PROFILE_SCOPE("bgfx.frame");
      if (!m_headless) m_renderer.frame(); }
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
    if (!m_cameraFinder.find(simWorld(), view, proj, aspect, clear,
                             m_renderer.homogeneousDepth()))
        return false;

    flecs::world* world = m_gameWorld ? m_gameWorld.get() : nullptr;
    ENGINE_PROFILE_SCOPE("Render");
    m_renderer.renderToBackbuffer(view, proj, clear, world);
    return true;
}
