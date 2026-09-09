// ── EngineRuntime — SIMULATION phase ─────────────────────────────────────────
// One of runtime's four phase TUs (boot / frame / sim / lifecycle core in
// runtime.cpp). Play-session lifecycle (start/stop, snapshot worlds), the
// fixed-timestep loop, and the per-frame system tick.
// NO <bgfx/bgfx.h> — sim never touches the GPU.
#include "runtime/runtime.h"
#include "runtime/sim_classification.h"
#include "runtime/sim_command.h"
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
        simhash::registerClassification(*m_gameWorld);
        SceneSerializer::loadIntoWorld(m_simSnapshot, *m_gameWorld, storage);
    }

    m_simulating = true;
    m_simElapsed = 0.0;
    m_simFrame   = 0;
    // A fresh session starts with the window shut: between sessions, and
    // between ticks, a submission has no tick to belong to.
    m_commands.clear();
    m_commands.setSubmissionOpen(false);
    // Fresh session, fresh accumulator: a stop mid-step left a stale
    // fraction that fired the new session's first fixed step early and
    // handed the renderer a non-zero interpolation alpha on frame one
    // (visible jump) — audit H.5.
    m_simAccumulator = 0.0f;
    // Bind the script surface to the sim world BEFORE plugins start — Lua
    // instantiates script instances during broadcastSimStart.
    m_scriptHost->setWorld(&simWorld());
    // Locomotion composes only while there is a fixed step to compose it in.
    // Unbound at stop, below, so the editor's preview keeps the direct path.
    m_scriptHost->setCommandBuffer(&m_commands);
    m_stableIdCache.clear();          // a new world reuses flecs ids
    m_movesDispatched = 0;
    m_movesUnresolved = 0;
    m_teleportsDispatched = 0;
    m_intents.clear();
    m_intents.setSubmissionOpen(false);
    // A fresh session re-bases the look cursor: the totals are cumulative and
    // never reset, so carrying yesterday's cursor would hand tick one every
    // count the mouse produced while the editor was open.
    m_input.lookTotal(&m_lookCursorX, &m_lookCursorY);
    m_authority.reset();
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
    // ── Deferred callbacks run BEFORE the code they live in is unmapped ──────
    // engineJobsOnMain lets a kit defer work to the main thread, and the queue
    // holds a function pointer INTO the kit's dylib. pumpMain() only runs in
    // tickSystems, so anything queued after this frame's pump was still sitting
    // there when m_kits.stop() dlclosed the library — and the next frame's pump
    // jumped into unmapped memory. Draining here runs it while the kit is still
    // loaded, which is both safe and what the caller asked for.
    jobs::drainMain();
    // Kits detach + dlclose AFTER the stop broadcast (so onSimulationStop has
    // fired on them) and before the sim world is torn down below.
    m_kits.stop(m_plugins);
    m_scriptHost->setWorld(nullptr);            // C API + Lua go dormant
    // The snapshot world dies below — drop every cached query against it
    // (a future world could reuse the same address and false-match).
    m_cameraFinder.reset();
    m_eventSweeper.reset();                     // release queries before world dies
    m_animatorSystem.resetWorldCache();
    m_renderer->resetWorldCaches();
    m_spinnerQuery.reset();          // sim-world query — world dies below
    m_prevSnapQuery.reset();         // ditto: PrevTransform snapshot queries
    m_prevSnapAddQuery.reset();
    m_scriptHost->setPhysicsService(nullptr);
    m_scriptHost->setAudioService(nullptr);
    m_scriptHost->setCommandBuffer(nullptr);   // no fixed step, no composition
    m_stableIdCache.clear();                   // entries point into a dead world
    m_authority.reset();                       // its query outlives the world otherwise
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
        // ── The command window ─────────────────────────────────────────────
        // Open ONLY across broadcastUpdate. A submission from onFrame or an
        // editor panel would otherwise land in whichever tick's buffer was
        // open when the caller ran, making the tick's command set depend on
        // the frame rate — BUG-0053's defect class inside the subsystem meant
        // to remove it. Outside this window submit() refuses and counts.
        // ── Device -> Intent, ONCE PER TICK ────────────────────────────────
        // Before onUpdate, so gameplay reads intent that was sampled at the
        // simulation's rate rather than a device at the frame's. This is what
        // stage 4 said it did not fix: CameraLook stopped the render-rate write
        // reaching hashed state, and the SIMULATION still read a frame-rate
        // accumulation. See runtime/sim_intent.h.
        m_intents.setSubmissionOpen(true);
        { ENGINE_PROFILE_SCOPE("Sim.intent"); sampleLocalIntent(); }
        m_intents.sortForExecution();

        m_commands.setSubmissionOpen(true);
        // ── The authority baseline ─────────────────────────────────────────
        // Re-based around every LEGITIMATE writer and checked after every phase
        // that must not write, so a violation names the phase that made it
        // rather than "the world differs". See runtime/transform_authority.h.
        m_authority.rebase(w);
        { ENGINE_PROFILE_SCOPE("Sim.update");  m_plugins.broadcastUpdate(w, kSimDt); }
        m_commands.setSubmissionOpen(false);
        m_authority.check(w, "onUpdate");

        // ── The tick's commands become canonical HERE ───────────────────────
        // Everything gameplay asked for during onUpdate is now in the buffer,
        // and this puts it in an order that is a function of the commands —
        // (entity, source, kind, seq) — rather than of who submitted first.
        // That independence is the property last-writer-wins lacked.
        //
        // Ordered HERE rather than at the point of execution, deliberately: the
        // record and the execution must not be able to disagree, so what the
        // replay ring stores below is the sequence that actually ran. The
        // executor is dispatchMoves(), immediately after.
        m_commands.sortForExecution();

        // ── Stage 2: locomotion is COMPOSED, then dispatched ───────────────
        // Every MoveContribution aimed at a character folds into one movement
        // for that character, by the rule in runtime/move_compose.h, and the
        // physics service is called once per entity in ascending EntityId
        // order. Before this, JoltPlugin::charMove stored its argument and the
        // last caller in the tick erased the rest.
        // Teleport writes Transform, and legitimately: it is the command that
        // exists so gameplay does not have to write the field directly.
        { ENGINE_PROFILE_SCOPE("Sim.moves"); dispatchMoves(w); }
        m_authority.rebase(w);

        { ENGINE_PROFILE_SCOPE("Sim.physics"); m_plugins.broadcastPhysicsStep(w, kSimDt); }
        m_authority.rebase(w);          // physics owns these fields; it just wrote them
        { ENGINE_PROFILE_SCOPE("Sim.post");    m_plugins.broadcastPostPhysics(w); }
        m_authority.check(w, "onPostPhysics");

        // ── Gameplay clocks, at kSimDt, INSIDE the step ────────────────────
        // Both of these used to run once per FRAME with the frame's dt, so the
        // components they write — Transform for the spinner, Animator::time for
        // the animator — advanced at render rate. The same content simulated at
        // 1 frame/tick and 2 frames/tick produced different world state on the
        // very first tick.
        //
        // tests/determinism_gate_test.cpp measures BOTH, in separate tiers, and
        // both are in its gating lane. The spinner is what it found first, and
        // it is the engine's ONE demo gameplay system; the animator tier was
        // added afterwards, when a review pointed out that this comment had
        // been claiming measurement for a tier that did not exist.
        //
        // ADVANCE only. Sampling the pose is presentation and stays on the
        // frame (see AnimatorSystem::advance/sample) — which also means a hitch
        // that runs four fixed steps advances four clocks and samples one pose,
        // instead of doing the expensive half four times over.
        { ENGINE_PROFILE_SCOPE("Sim.spinner"); stepSpinners(w, kSimDt); }
        m_authority.check(w, "stepSpinners");
        { ENGINE_PROFILE_SCOPE("Sim.animClock");
          if (m_gameWorld) m_animatorSystem.advance(w, kSimDt);
          else             m_animatorSystem.advance(kSimDt); }
        // The flecs pipeline is part of the simulation, so it belongs here at a
        // fixed dt rather than outside at the frame's. Zero systems are
        // registered today, which is exactly why moving it is cheap now.
        { ENGINE_PROFILE_SCOPE("Sim.progress"); w.progress(kSimDt); }

        // Record what this tick was told to do, then start the next tick's
        // buffer empty. The ring is bounded; see EngineRuntime::m_cmdRing.
        recordTickCommands();
        m_commands.clear();
        // Intents stay open across onUpdate — a kit or an AI system submits its
        // own alongside the sampled one — and close with the tick.
        m_intents.clear();
    }

    m_renderer->setSimAlpha(m_simAccumulator / kSimDt);   // leftover fraction

    // Render-rate hook: presentation work (late-latched camera) runs once
    // per FRAME with real dt, after the fixed steps — this is what keeps
    // look latency at frame rate even when sim ticks slower.
    { ENGINE_PROFILE_SCOPE("Sim.frame"); m_plugins.broadcastFrame(w, dt); }
    // Render rate, so a write here is frame-rate-coupled as well as
    // unauthorised — BUG-0053's class on top of an ownership violation.
    m_authority.check(w, "onFrame");

    // Snapshot mode runs in a separate world that tickSystems never touches —
    // run animation and the flecs pipeline here so play mode behaves exactly
    // like in-place simulation. (In-place mode: simWorld() == m_ecs, which
    // tickSystems already animates and progresses — don't double-tick.)
    if (m_gameWorld) {
        // SAMPLE only — the clock advanced inside the fixed loop above, and
        // m_gameWorld->progress() moved there with it. Calling tick() here
        // would advance Animator::time a second time, at frame rate, which is
        // the defect this change exists to remove.
        m_animatorSystem.sample(*m_gameWorld);
    }
}

// ONE body for the spinner, called from two places at two rates: the fixed step
// during a session, the frame when previewing in the editor. Two copies of this
// loop is how the two would drift — the same argument extraction makes for
// "ONE body, serial or parallel".
// ── The replay ring ─────────────────────────────────────────────────────────
// A bounded copy of what each recent tick was told to do. Bounded because an
// unbounded record of a long session is a leak with a respectable name; a ring
// is also exactly the shape rollback wants (re-simulate the last N ticks).
//
// OFF BY DEFAULT. Recording every tick costs a vector copy per tick and is only
// wanted by a test, a replay tool, or a netcode client — the same reasoning
// that keeps the profiler and the memory counters opt-in.
void EngineRuntime::setCommandRecording(bool on, size_t ticks) {
    m_cmdRecording = on;
    m_cmdRing.assign(on ? (ticks ? ticks : 1) : 0, {});
    m_cmdRingHead = 0;
}

// ── Resolving a command's target ────────────────────────────────────────────
// Commands name entities by EntityId because a command stream outlives the
// process; the simulation needs the flecs entity. findById is O(n) and says in
// its own header never to call it in a loop, so this memoises.
//
// A cached entry can go stale two ways — the entity is destroyed, or its
// EntityId changes — and both are checked on every hit rather than trusted,
// because a stale hit would silently move the WRONG entity. That is a worse
// failure than the miss it saves.
flecs::entity EngineRuntime::resolveStableId(flecs::world& w, uint64_t id) {
    if (id == 0) return flecs::entity{};
    auto it = m_stableIdCache.find(id);
    if (it != m_stableIdCache.end()) {
        flecs::entity e = w.entity(it->second);
        if (e.is_alive()) {
            const EntityId* got = e.try_get<EntityId>();
            if (got && got->value == id) return e;
        }
        m_stableIdCache.erase(it);
    }
    flecs::entity found = findById(w, id);
    if (found) m_stableIdCache[id] = found.id();
    return found;
}

// ── Device -> Intent, once per fixed step ───────────────────────────────────
// The whole point of the layer: this runs INSIDE the fixed step, so what a
// controller reads is a function of the tick and not of how many frames the
// tick happened to be split into.
//
// It is a no-op until a game names its controller (setLocalController) — a
// headless server has no local device, and a host driving intent from the
// network wants to fill the buffer itself rather than have a device
// contribute alongside it.
void EngineRuntime::sampleLocalIntent() {
    if (m_localController == 0) return;

    simintent::Intent in{};
    in.entity = m_localController;
    in.source = 0;                       // simcmd::Source::Gameplay — the player

    // ── The look delta, diffed against OUR OWN cursor ───────────────────────
    // NOT consumeLook(). That drains a single shared cursor, so calling it here
    // would silently starve a kit that also calls it — input_manager.h states
    // exactly this and prescribes diffing lookTotal for a second consumer.
    //
    // Diffing per TICK is what makes this frame-rate-independent: the totals
    // only ever grow, so whatever the mouse produced between the last tick and
    // this one lands in this tick, whether that was one pump or four.
    double lx = 0.0, ly = 0.0;
    m_input.lookTotal(&lx, &ly);
    in.lookDx = (float)(lx - m_lookCursorX);
    in.lookDy = (float)(ly - m_lookCursorY);
    m_lookCursorX = lx;
    m_lookCursorY = ly;

    // Axes and actions through the action map, so intent is in the GAME's terms
    // — a recorded stream survives a re-bind, and an AI can produce the same
    // struct with no device at all.
    m_input.axis2("Move", &in.moveX, &in.moveY);

    const auto& names = m_actionSet.names();
    for (size_t i = 0; i < names.size(); ++i) {
        const char* n = names[i].c_str();
        const uint32_t bit = 1u << i;
        if (m_input.actionDown(n))     in.held     |= bit;
        if (m_input.actionPressed(n))  in.pressed  |= bit;
        if (m_input.actionReleased(n)) in.released |= bit;
    }

    m_intents.submit(in);
}

void EngineRuntime::dispatchMoves(flecs::world& w) {
    if (m_commands.size() == 0) return;
    const PhysicsServiceRef* pr = w.try_get<PhysicsServiceRef>();
    if (!pr || !pr->svc) return;      // no physics attached; nothing to drive

    simcmd::composeMoves(m_commands.commands(), m_resolvedMoves);
    for (const simcmd::ResolvedMove& r : m_resolvedMoves) {
        flecs::entity e = resolveStableId(w, r.entity);
        if (!e) { ++m_movesUnresolved; continue; }   // named a dead entity
        if (r.exclusiveConflicts || r.overrideConflicts) {
            // Two systems at the SAME priority fighting over one character.
            // The fold resolved it the same way it always will, so this is not
            // a determinism problem — it is a content bug, and the only place
            // it can be seen is here.
            LOG_WARN("SimCmd", "entity %llu received %u conflicting exclusive "
                     "and %u conflicting override movement contributions at "
                     "equal priority — lower seq won",
                     (unsigned long long)r.entity,
                     (unsigned)r.exclusiveConflicts,
                     (unsigned)r.overrideConflicts);
        }
        // NOTE: r.vertical is composed and NOT delivered. charMove carries only
        // the horizontal pair, and vertical velocity is owned by charJump and
        // gravity inside the character update. Root motion needs it, and it
        // arrives with the physics service group stage 3 adds — composing it
        // now costs nothing and keeps the rule whole; claiming it works would
        // not be true.
        ++m_movesDispatched;
        pr->svc->charMove(w, e.id(), r.horizX, r.horizZ);
    }

    // ── Teleports ───────────────────────────────────────────────────────────
    // In the buffer's canonical order, so two teleports of one entity in one
    // tick resolve the same way every run. Executed AFTER the movement fold on
    // purpose: a teleport is a discontinuity, and the tick's steering should
    // not be applied on top of a destination it was never computed for.
    for (const simcmd::SimCommand& c : m_commands.commands()) {
        if (c.kind != simcmd::Cmd::Teleport) continue;
        flecs::entity e = resolveStableId(w, c.entity);
        if (!e) { ++m_movesUnresolved; continue; }

        const float x = c.a[simcmd::tele::kSlotX];
        const float y = c.a[simcmd::tele::kSlotY];
        const float z = c.a[simcmd::tele::kSlotZ];
        const bool  haveRot = simcmd::tele::hasRotation(c);
        const float q[4] = { c.a[simcmd::tele::kSlotQX], c.a[simcmd::tele::kSlotQY],
                             c.a[simcmd::tele::kSlotQZ], c.a[simcmd::tele::kSlotQW] };

        // The backend moves its own body; the ECS state below is this layer's
        // to write, because a plugin writing Transform is the second authority
        // this architecture exists to remove. A teleport of an entity with no
        // body is still a legal teleport — it just moves the Transform.
        pr->svc->teleport(w, e.id(), x, y, z, haveRot ? q : nullptr);

        if (Transform* t = e.try_get_mut<Transform>()) {
            t->position = { x, y, z };
            if (haveRot) t->rotation = { q[0], q[1], q[2], q[3] };
            // ── AND THE INTERPOLATION HISTORY, which is the whole reason this
            // is atomic. PrevTransform is snapshotted at the TOP of the fixed
            // step, so without this the renderer would blend from where the
            // entity was to where it now is and draw it sliding across the gap
            // — the streak that makes a teleport look like a bug even when the
            // simulation is correct.
            if (PrevTransform* p = e.try_get_mut<PrevTransform>()) {
                p->position = t->position;
                p->rotation = t->rotation;
            }
        }
        ++m_teleportsDispatched;
    }
}

void EngineRuntime::recordTickCommands() {
    if (!m_cmdRecording || m_cmdRing.empty()) return;
    // Already sorted into canonical order, so what is stored is the order the
    // tick will EXECUTE in, not the order things happened to be submitted.
    // The tick number goes in with it: a stream whose slots cannot say which
    // tick they are can only be checked against per-tick state hashes by
    // assuming no gaps, and gaps are exactly what a hitch produces.
    m_cmdRing[m_cmdRingHead] =
        { m_simFrame, m_intents.intents(), m_commands.commands() };
    m_cmdRingHead = (m_cmdRingHead + 1) % m_cmdRing.size();
}

const EngineRuntime::RecordedTick&
EngineRuntime::recordedTick(size_t ticksAgo) const {
    static const RecordedTick kEmpty;
    if (!m_cmdRecording || m_cmdRing.empty() || ticksAgo >= m_cmdRing.size())
        return kEmpty;
    // head points at the NEXT slot to write, so the tick just completed is
    // head-1. Unsigned arithmetic, hence the += size before the modulo.
    const size_t i = (m_cmdRingHead + m_cmdRing.size() - 1 - ticksAgo)
                   % m_cmdRing.size();
    return m_cmdRing[i];
}

void EngineRuntime::stepSpinners(flecs::world& w, float dt) {
    m_spinnerQuery.get(w)
        .each([dt](flecs::entity, Transform& t, const Spinner& s) {
            const bx::Quaternion qY = bx::fromAxisAngle({0,1,0}, s.speedYaw   * dt);
            const bx::Quaternion qP = bx::fromAxisAngle({1,0,0}, s.speedPitch * dt);
            t.rotation = bx::normalize(bx::mul(qP, bx::mul(qY, t.rotation)));
        });
}

void EngineRuntime::tickSystems(float dt, bool paused) {
    if (!m_initialized) { LOG_ERROR("Runtime", "tick() before init()"); return; }
    jobs::pumpMain();   // drain job->main-thread requests; sweep finished jobs
    // Input: drain sources, mirror UI/focus gates, fold a tick snapshot.
    // (Per-frame tick today; slots into the fixed-timestep loop when it lands.)
    // ── A HEADLESS HOST HAS NO WINDOW TO BE FOCUSED ─────────────────────────
    // wsi::isFocused on a null window reads as "not focused", and
    // InputManager::accept drops every press and every motion event while
    // unfocused (correctly — a game must not act on input meant for another
    // window). So a headless host silently discarded ALL input, with no
    // diagnostic: a server replaying a recorded stream, or a determinism tier
    // driving one, would receive nothing and look like it simply did nothing.
    // Found by the stage-5 intent test, which measured zero motion through a
    // ReplaySource that was delivering plenty.
    //
    // Without a window there is no other window for the input to belong to,
    // so the gate has nothing to protect and the honest answer is "focused".
    m_input.setFocused(m_headless ? true : InputSystem::get().windowFocused());
    m_input.setUICapture(InputSystem::get().uiCapturesKeyboard(),
                         InputSystem::get().uiCapturesMouse());
    m_input.pump();
    if (!m_simulating) m_input.beginTick(hid::nowNs());
    // ── EDITOR PREVIEW ONLY, and the guard is the whole point ───────────────
    // Spinner writes Transform, a component the determinism gate hashes. Run at
    // FRAME dt it advanced at render rate, so identical content simulated at
    // two different frame rates diverged on the first tick. While a simulation
    // is running it now advances in the fixed step (tickSimulation), once per
    // kSimDt.
    //
    // It still runs here when NOT simulating, because that is the editor
    // viewport preview and there is no fixed step to hang it on — the same
    // split animation has always had ("animation runs even when gameplay
    // systems are paused"). Nothing hashes the edit world outside a session.
    if (!paused && !m_simulating) {
        // Spin in the world the user is LOOKING AT: the snapshot game world
        // during Play, the edit world otherwise. The old query was built
        // once on m_ecs, so Snapshot-play spinners froze while the hidden
        // edit world kept animating (audit H.2).
        stepSpinners(simWorld(), dt);
    }
    // Animation runs even when gameplay systems are paused — the editor
    // scrubber and preview should always animate. During Snapshot play the
    // game world is animated per fixed step in tickSimulation; sampling the
    // hidden edit world's animators too was pure waste (audit H.2).
    // SAMPLE at frame rate; ADVANCE only when there is no fixed step to do it
    // (editor preview). During a session the clock moved in tickSimulation.
    { ENGINE_PROFILE_SCOPE("Animation");
      if (!m_gameWorld) {
          if (m_simulating) m_animatorSystem.sample();
          else              m_animatorSystem.tick(dt);
      } }
    // Edit world stays serviced even during Snapshot play — editor panels
    // still operate on it (deferred ops, observers); progress when idle is
    // near-free. The game world progresses at fixed dt in tickSimulation.
    // An EXPLICIT dt. progress() with no argument means delta_time = 0, which
    // flecs documents as "automatically measure the time passed since the last
    // frame" — a wall clock reaching the ECS pipeline. During a session the sim
    // world progresses in the fixed step instead; this call keeps servicing the
    // EDIT world (deferred ops, observers, editor panels), which is why it is
    // skipped when the edit world IS the sim world.
    if (!(m_simulating && !m_gameWorld)) {
        ENGINE_PROFILE_SCOPE("ECS.progress");
        m_ecs.progress(dt);
    }
}
