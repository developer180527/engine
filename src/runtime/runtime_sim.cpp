// ── EngineRuntime — SIMULATION phase ─────────────────────────────────────────
// One of runtime's four phase TUs (boot / frame / sim / lifecycle core in
// runtime.cpp). Play-session lifecycle (start/stop, snapshot worlds), the
// fixed-timestep loop, and the per-frame system tick.
// NO <bgfx/bgfx.h> — sim never touches the GPU.
#include "runtime/runtime.h"
#include "core/logger.h"
#include "runtime/scripting/script_host.h"
#include "runtime/scripting/script_services.h"
#include "runtime/jobs/jobs.h"
#include "runtime/input/input_system.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"

#include <bx/math.h>

#include "assets/asset_storage.h"
#include "scene/scene_serializer.h"
#include "scene/reflected_serde.h"
#include "components/meta_registry.h"
#include "components/prev_transform.h"
#include "components/camera.h"
#include "core/transform.h"

bool EngineRuntime::startSimulation(SimMode mode) {
    if (!m_initialized) {
        LOG_ERROR("Sim", "startSimulation() before init()");
        return false;
    }
    if (m_simulating) return true;
    // Lifecycle order is onAttach -> onSimulationStart; broadcasting sim
    // start to plugins whose onAttach never ran silently violates it. The
    // class enforces every other lifecycle rule loudly — this one too
    // (audit M.5).
    if (!m_pluginsAttached)
        LOG_WARN("Sim", "startSimulation() before attachPlugins() — plugins "
                 "will receive onSimulationStart without onAttach (lifecycle "
                 "violation; call attachPlugins() first)");

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
    // Fresh session, fresh accumulator: a stop mid-step left a stale
    // fraction that fired the new session's first fixed step early and
    // handed the renderer a non-zero interpolation alpha on frame one
    // (visible jump) — audit H.5.
    m_simAccumulator = 0.0f;
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
    m_eventSweeper.reset();                     // release queries before world dies
    m_animatorSystem.resetWorldCache();
    m_renderer.resetWorldCaches();
    m_spinnerQuery.reset();          // sim-world query — world dies below
    m_prevSnapQuery.reset();         // ditto: PrevTransform snapshot queries
    m_prevSnapAddQuery.reset();
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
        // Age event components BEFORE any broadcast: a message written last tick
        // is guaranteed present for this whole tick regardless of who wrote or
        // reads it first (see event_sweeper.h), decoupling kits from load order.
        { ENGINE_PROFILE_SCOPE("Sim.events"); m_eventSweeper.sweep(w); }
        // Interpolation snapshot: remember where everything WAS before this
        // step so rendering can lerp. Cameras are excluded — their rotation
        // is late-latched at render rate (onFrame) and must not lag.
        { ENGINE_PROFILE_SCOPE("Sim.prevSnapshot");
        // Entities that already have a PrevTransform — everything, after its
        // first step. Overwriting fields in place: no defer, no command buffer,
        // no per-entity lookup. This is the pass that runs 60 times a second.
        m_prevSnapQuery.get(w, [](auto& b) { b.template without<Camera>(); })
            .each([](const Transform& t, PrevTransform& p) {
                p.position = t.position;
                p.rotation = t.rotation;
                p.scale    = t.scale;
            });
        // Newcomers: the ONLY case that needs a structural add, and normally
        // empty. Deferred because set<> during iteration is a structural op.
        auto& addQ = m_prevSnapAddQuery.get(w, [](auto& b) {
            b.template without<PrevTransform>().template without<Camera>(); });
        w.defer_begin();
        addQ.each([](flecs::entity e, const Transform& t) {
            e.set<PrevTransform>({ t.position, t.rotation, t.scale });
        });
        w.defer_end(); }
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
        // Spin in the world the user is LOOKING AT: the snapshot game world
        // during Play, the edit world otherwise. The old query was built
        // once on m_ecs, so Snapshot-play spinners froze while the hidden
        // edit world kept animating (audit H.2).
        m_spinnerQuery.get(simWorld())
            .each([dt](flecs::entity, Transform& t, const Spinner& s) {
                const bx::Quaternion qY =
                    bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
                const bx::Quaternion qP =
                    bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);
                t.rotation = bx::normalize(bx::mul(qP, bx::mul(qY, t.rotation)));
            });
    }
    // Animation runs even when gameplay systems are paused — the editor
    // scrubber and preview should always animate. During Snapshot play the
    // game world is animated per fixed step in tickSimulation; sampling the
    // hidden edit world's animators too was pure waste (audit H.2).
    { ENGINE_PROFILE_SCOPE("Animation");
      if (!m_gameWorld) m_animatorSystem.tick(dt); }
    // Edit world stays serviced even during Snapshot play — editor panels
    // still operate on it (deferred ops, observers); progress when idle is
    // near-free. The game world progresses at fixed dt in tickSimulation.
    { ENGINE_PROFILE_SCOPE("ECS.progress"); m_ecs.progress(); }
}
