#pragma once
#include <memory>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <thread>

#include "runtime/plugin.h"
#include "core/logger.h"
#include "core/memory/mem.h"
#include "components/collision_events.h"
#include <mutex>
#include <unordered_map>
#include "components/rigid_body.h"
#include "components/character_controller.h"
#include "core/transform.h"
#include "core/transform_utils.h"

// Jolt headers — Jolt.h must be first
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include "plugins/jolt_jobs_adapter.h"   // physics on the engine job pool
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "runtime/scripting/script_services.h"
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "runtime/scripting/script_services.h"
#include <algorithm>

// ── Physics layers ─────────────────────────────────────────────────────────
namespace PhysLayers {
    static constexpr JPH::ObjectLayer STATIC  = 0;
    static constexpr JPH::ObjectLayer DYNAMIC = 1;
    static constexpr JPH::uint        COUNT   = 2;
}
namespace BPLayers {
    static constexpr JPH::BroadPhaseLayer STATIC  {0};
    static constexpr JPH::BroadPhaseLayer DYNAMIC {1};
    static constexpr JPH::uint            COUNT   = 2;
}

// ── BodyID hasher (v5.2.0 has no built-in hasher) ─────────────────────────
struct BodyIDHash {
    size_t operator()(JPH::BodyID id) const noexcept {
        return std::hash<uint32_t>()(id.GetIndexAndSequenceNumber());
    }
};

// ── Required Jolt interface implementations ────────────────────────────────
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        m_map[PhysLayers::STATIC]  = BPLayers::STATIC;
        m_map[PhysLayers::DYNAMIC] = BPLayers::DYNAMIC;
    }
    JPH::uint            GetNumBroadPhaseLayers()                const override { return BPLayers::COUNT; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l)  const override { return m_map[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l)   const override {
        return (l == BPLayers::STATIC) ? "STATIC" : "DYNAMIC";
    }
#endif
private:
    JPH::BroadPhaseLayer m_map[PhysLayers::COUNT];
};

class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        if (obj == PhysLayers::STATIC)  return (bp == BPLayers::DYNAMIC);
        if (obj == PhysLayers::DYNAMIC) return true;
        return false;
    }
};

class ObjLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == PhysLayers::STATIC && b == PhysLayers::STATIC);
    }
};

// ── Collision event pair ──────────────────────────────────────────────────
struct CollisionPair { JPH::BodyID a, b; bool enter; };

// ── JoltPlugin ─────────────────────────────────────────────────────────────
class JoltPlugin final : public IEnginePlugin, public IPhysicsService {
public:
    const char* name()    const override { return "Physics"; }
    const char* version() const override { return "0.1.0-jolt5"; }

    void onAttach(RuntimeContext&) override {
        // All Jolt allocations land in the Physics heap (function pointers,
        // set before the Factory — the first thing Jolt allocates).
        JPH::Allocate   = [](size_t s) { return mem::alloc(s, 16, mem::Tag::Physics); };
        JPH::Reallocate = [](void* p, size_t, size_t n) {
            return p ? mem::realloc(p, n) : mem::alloc(n, 16, mem::Tag::Physics);
        };
        JPH::Free            = [](void* p) { mem::free(p); };
        JPH::AlignedAllocate = [](size_t s, size_t a) { return mem::alloc(s, a, mem::Tag::Physics); };
        JPH::AlignedFree     = [](void* p) { mem::free(p); };
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        LOG_SUCCESS("Physics", "Jolt %d.%d.%d attached",
            JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH);
    }

    void onDetach() override {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
        LOG_INFO("Physics", "Jolt detached");
    }

    void onSimulationStart(flecs::world& ecs) override {
        m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
        // Physics runs on the ENGINE pool (see jolt_jobs_adapter.h) — Jolt
        // spawns no threads of its own.
        m_jobSystem     = std::make_unique<JoltJobsAdapter>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);

        m_physics = std::make_unique<JPH::PhysicsSystem>();
        m_physics->Init(4096, 0, 4096, 4096,
            m_bpInterface, m_objVsBP, m_objPairFilter);
        m_physics->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        ecs.set<PhysicsServiceRef>({ this });   // publish to the script backend

        ecs.query_builder<const Transform, const RigidBody>().build()
            .each([this](flecs::entity e, const Transform& t, const RigidBody& rb) {
                spawnBody(e, t, rb);
            });

        ecs.query_builder<const Transform, const CharacterController>().build()
            .each([this](flecs::entity e, const Transform& t, const CharacterController& cc) {
                spawnCharacter(e, t, cc);
            });

        // Cached for the per-step runtime sync (clones/spawns during play).
        m_bodySyncQ = ecs.query_builder<const Transform, const RigidBody>().build();
        m_charSyncQ = ecs.query_builder<const Transform, const CharacterController>().build();

        m_contactListener.owner = this;
        m_physics->SetContactListener(&m_contactListener);
        m_physics->OptimizeBroadPhase();
        LOG_SUCCESS("Physics", "Simulation start — %d bodies", (int)m_entityToBody.size());
    }

    void onSimulationStop() override {
        if (!m_physics) return;
        // ── DRAIN FIRST, while everything the jobs touch is still alive ─────
        // QueueJob is fire-and-forget, so a `physics.job` can still be running
        // here. It holds a Job* from the adapter's free list and steps a
        // PhysicsSystem this function is about to destroy, so the wait belongs
        // BEFORE both — not at m_jobSystem.reset() further down, which would
        // free m_physics out from under it.
        //
        // On a stall we LEAK the adapter rather than free it under a live job.
        // The alternative is aborting, and this runs from the editor's Stop
        // button: killing the process there costs the user their unsaved scene,
        // while leaking one job list costs a few KB in a session that is
        // already in trouble.
        //
        // ── WHAT THE LEAK ACTUALLY BUYS, stated exactly ────────────────────
        // It keeps alive what the adapter owns: the Job the worker is inside,
        // the free list backing it, m_queued and m_deferred. It does NOT make
        // a stalled job safe, and an earlier version of this comment implied it
        // did. That job runs
        //
        //     step->mContext->mPhysicsSystem->JobFindCollisions(step, ...)
        //
        // which also reaches m_physics — reset a few lines below — and `step`,
        // a PhysicsUpdateContext that lived on PhysicsSystem::Update's stack
        // and was gone before this function was ever called. Leaking m_physics
        // too would not close that: the context dangles first.
        //
        // So the honest framing is that a 30-second stall is already undefined
        // behaviour, and this removes ONE of several dangling pointers. It is
        // chosen not because it makes the failure safe but because it makes it
        // survivable often enough for the user to save, where abort never is.
        if (m_jobSystem && !m_jobSystem->drain()) {
            LOG_ERROR("Physics", "physics jobs did not finish in 30s — leaking "
                                 "the job adapter rather than freeing it under "
                                 "them. This removes ONE dangling pointer, not "
                                 "all of them: the pool is stuck and the "
                                 "process is already in undefined behaviour. "
                                 "Save your work and restart.");
            (void)m_jobSystem.release();     // deliberate, see above
        }
        auto& bi = m_physics->GetBodyInterface();
        for (auto& [eid, bid] : m_entityToBody) {
            bi.RemoveBody(bid);
            bi.DestroyBody(bid);
        }
        m_entityToBody.clear();
        m_bodyToEntity.clear();
        m_characters.clear();   // JPH::Ref releases each CharacterVirtual
        m_charState.clear();
        m_bodySyncQ = {};        // queries must die before their world
        m_charSyncQ = {};
        m_physics.reset();
        m_jobSystem.reset();     // already drained above; its dtor's wait is a no-op
        m_tempAllocator.reset();
        m_accumulator = 0.0f;
        LOG_INFO("Physics", "Simulation stop");
    }

    void onPhysicsStep(flecs::world& ecs, float dt) override {
        if (!m_physics) return;
        syncRuntimeBodies(ecs);   // entities cloned/destroyed DURING play
        m_accumulator += dt;
        int steps = 0;
        while (m_accumulator >= kFixedDt && steps < 4) {
            // ECS -> physics, once per SUBSTEP and not once per call. A
            // kinematic target is reached by velocity over the substep's dt, so
            // pushing it once and then stepping four times would overshoot by
            // three: the body would keep the velocity that got it there. Re-
            // targeting each substep drives it to zero on arrival instead.
            pushEcsToPhysics(ecs);
            m_physics->Update(kFixedDt, 1,
                m_tempAllocator.get(), m_jobSystem.get());
            updateCharacters(kFixedDt);
            m_accumulator -= kFixedDt;
            ++steps;
        }
        writeBackTransforms(ecs);
        writeBackCharacters(ecs);
    }

    // Bodies are born at sim start — but gameplay CLONES entities mid-play
    // (spawners) and destroys them (deaths). Without this sync, runtime
    // spawns are GHOSTS (raycasts pass through: "shot -> miss" on spawned
    // zombies) and despawned entities leave invisible colliders behind.
    void syncRuntimeBodies(flecs::world& ecs) {
        m_bodySyncQ.each([this](flecs::entity e, const Transform& t,
                                const RigidBody& rb) {
            if (!m_entityToBody.count(e.id())) spawnBody(e, t, rb);
        });
        m_charSyncQ.each([this](flecs::entity e, const Transform& t,
                                const CharacterController& cc) {
            if (!m_characters.count(e.id())) spawnCharacter(e, t, cc);
        });
        auto& bi = m_physics->GetBodyInterface();
        for (auto it = m_entityToBody.begin(); it != m_entityToBody.end();) {
            if (!ecs.entity(it->first).is_alive()) {
                m_bodyToEntity.erase(it->second);
                bi.RemoveBody(it->second);
                bi.DestroyBody(it->second);
                it = m_entityToBody.erase(it);
            } else ++it;
        }
        for (auto it = m_characters.begin(); it != m_characters.end();) {
            if (!ecs.entity(it->first).is_alive()) {
                m_charState.erase(it->first);
                it = m_characters.erase(it);
            } else ++it;
        }
    }

    // Post-step: publish this frame's contacts as CollisionEvents components.
    // Runs before LuaScriptPlugin::onPostPhysics (Physics is registered first),
    // so scripts read a fresh, complete event set.
    void onPostPhysics(flecs::world& ecs) override {
        if (!m_physics) return;
        flushCollisionEvents(ecs);
    }

    // ── Stats (UI-free — the editor's Plugins panel reads these) ────────
    bool simulationActive() const { return m_physics != nullptr; }
    int  bodyCount()        const { return (int)m_entityToBody.size(); }
    static constexpr float fixedTimestep() { return kFixedDt; }

    // ── IPhysicsService (scripts reach these through ScriptHost) ────────
    // Bodies are keyed by entity_t, so these need only the id — the world
    // arg is unused here (a backend that touched components would resolve it).
    void applyImpulse(flecs::world&, flecs::entity_t e, float x, float y, float z) override {
        if (!m_physics) return;
        auto it = m_entityToBody.find(e);
        if (it == m_entityToBody.end()) return;
        auto& bi = m_physics->GetBodyInterface();
        bi.ActivateBody(it->second);
        bi.AddImpulse(it->second, JPH::Vec3(x, y, z));
    }
    void setVelocity(flecs::world&, flecs::entity_t e, float x, float y, float z) override {
        if (!m_physics) return;
        auto it = m_entityToBody.find(e);
        if (it == m_entityToBody.end()) return;
        auto& bi = m_physics->GetBodyInterface();
        bi.ActivateBody(it->second);
        bi.SetLinearVelocity(it->second, JPH::Vec3(x, y, z));
    }
    bool getVelocity(flecs::world&, flecs::entity_t e, float& x, float& y, float& z) override {
        if (!m_physics) return false;
        auto it = m_entityToBody.find(e);
        if (it == m_entityToBody.end()) return false;
        JPH::Vec3 v = m_physics->GetBodyInterface().GetLinearVelocity(it->second);
        x = v.GetX(); y = v.GetY(); z = v.GetZ();
        return true;
    }
    RaycastHit raycast(float ox, float oy, float oz,
                       float dx, float dy, float dz, float maxDist) override {
        RaycastHit out;
        if (!m_physics) return out;
        JPH::Vec3 dir(dx, dy, dz);
        float len = dir.Length();
        if (len < 1e-6f || maxDist <= 0.0f) return out;
        dir = dir / len;                                       // normalize, extend by maxDist
        JPH::RRayCast ray(JPH::RVec3(ox, oy, oz), dir * maxDist);
        JPH::RayCastResult res;
        if (!m_physics->GetNarrowPhaseQuery().CastRay(ray, res)) return out;

        out.hit      = true;
        out.distance = res.mFraction * maxDist;
        JPH::RVec3 pt = ray.GetPointOnRay(res.mFraction);
        out.point[0] = (float)pt.GetX(); out.point[1] = (float)pt.GetY(); out.point[2] = (float)pt.GetZ();

        JPH::BodyLockRead lock(m_physics->GetBodyLockInterface(), res.mBodyID);
        if (lock.Succeeded()) {
            JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(res.mSubShapeID2, pt);
            out.normal[0] = n.GetX(); out.normal[1] = n.GetY(); out.normal[2] = n.GetZ();
        }
        auto it = m_bodyToEntity.find(res.mBodyID);
        out.entity = (it != m_bodyToEntity.end()) ? (uint64_t)it->second : 0;
        return out;
    }

    // Diagnostic: the rotation the CharacterVirtual is ACTUALLY holding, which
    // is the only way to see BUG-0059 from outside. The controller's shape is a
    // centred capsule, radially symmetric about the axis anything upright turns
    // around, so a rotation that never arrives changes no collision result a
    // test could observe — which is exactly why the bug survived. Returns false
    // when the entity has no character.
    bool characterRotation(flecs::entity_t e, float qXYZW[4]) const {
        auto it = m_characters.find(e);
        if (it == m_characters.end()) return false;
        const JPH::Quat q = it->second->GetRotation();
        qXYZW[0] = q.GetX(); qXYZW[1] = q.GetY();
        qXYZW[2] = q.GetZ(); qXYZW[3] = q.GetW();
        return true;
    }

    // ── Teleport (BUG-0057's answer) ────────────────────────────────────
    // Moves the BACKEND's pose only; the caller updates Transform and the
    // interpolation history, because those are ECS state this plugin should not
    // be the second writer of. Velocity is cleared in both branches — for a
    // dynamic body because momentum carried across a teleport is a bug, and for
    // a character because a fall in progress must not resume at the
    // destination.
    bool teleport(flecs::world& w, flecs::entity_t e,
                  float x, float y, float z, const float* q) override {
        if (!m_physics) return false;
        if (auto ch = m_characters.find(e); ch != m_characters.end()) {
            // The character's reference point is the FEET, footOffset below the
            // render origin — the same conversion spawnCharacter makes, and
            // writeBackCharacters undoes. Teleporting to the raw position would
            // sink or float the character by that offset every time.
            float foot = 0.0f;
            if (const CharacterController* cc =
                    w.entity(e).try_get<CharacterController>())
                foot = cc->footOffset;
            ch->second->SetPosition(JPH::RVec3(x, y - foot, z));
            ch->second->SetLinearVelocity(JPH::Vec3::sZero());
            if (auto st = m_charState.find(e); st != m_charState.end()) {
                st->second.vertVel      = 0.0f;
                st->second.desiredHoriz = JPH::Vec3::sZero();
            }
            return true;
        }
        auto it = m_entityToBody.find(e);
        if (it == m_entityToBody.end()) return false;
        auto& bi = m_physics->GetBodyInterface();
        const JPH::Quat rot = q ? JPH::Quat(q[0], q[1], q[2], q[3])
                                : bi.GetRotation(it->second);
        // SetPositionAndRotation, NOT MoveKinematic: a teleport is a
        // discontinuity, and the whole point is that no velocity is implied by
        // it. Activated, so a sleeping body notices it has been moved.
        bi.SetPositionAndRotation(it->second, JPH::RVec3(x, y, z), rot,
                                  JPH::EActivation::Activate);
        bi.SetLinearVelocity(it->second, JPH::Vec3::sZero());
        bi.SetAngularVelocity(it->second, JPH::Vec3::sZero());
        return true;
    }

    // ── Character controller service (scripts -> ScriptHost -> here) ────
    void charMove(flecs::world&, flecs::entity_t e, float vx, float vz) override {
        auto it = m_charState.find(e);
        if (it != m_charState.end()) it->second.desiredHoriz = JPH::Vec3(vx, 0, vz);
    }
    void charJump(flecs::world&, flecs::entity_t e, float speed) override {
        auto it = m_charState.find(e);
        if (it != m_charState.end() && it->second.grounded) it->second.vertVel = speed;
    }
    bool charIsGrounded(flecs::world&, flecs::entity_t e) override {
        auto it = m_charState.find(e);
        return it != m_charState.end() && it->second.grounded;
    }

    static constexpr float kFixedDt = 1.0f / 60.0f;

    BPLayerInterfaceImpl m_bpInterface;
    ObjVsBPFilter        m_objVsBP;
    ObjLayerPairFilter   m_objPairFilter;

    std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
    std::unique_ptr<JoltJobsAdapter>        m_jobSystem;
    std::unique_ptr<JPH::PhysicsSystem>       m_physics;
    float                                      m_accumulator = 0.0f;

    flecs::query<const Transform, const RigidBody>           m_bodySyncQ;
    flecs::query<const Transform, const CharacterController> m_charSyncQ;
    // ── std::map, NOT unordered_map, and the reason is ITERATION order ─────
    // ORDERED ONLY WHERE THE ORDER IS OBSERVED. Both maps below are walked to
    // DRIVE the simulation, not merely looked up: syncRuntimeBodies destroys
    // bodies in m_entityToBody's order — and Jolt's own docs (Architecture.md)
    // require bodies to be added and removed in the same order for determinism,
    // because BodyID recycling feeds its contact sort — while updateCharacters
    // steps characters in m_characters' order, where character-vs-character
    // interaction makes that order observable.
    //
    // ── WHAT THIS IS AND IS NOT EVIDENCE FOR (corrected 2026-09-08) ────────
    // The original note here said an unordered_map's order "differed between
    // two runs in the same process". MEASURED, and that is not true: libc++'s
    // std::hash<uint64_t> is the identity function and is not seeded, so for
    // the same keys inserted in the same order the bucket layout — and the
    // iteration order — is identical, run to run and even process to process.
    // Reverting either map to unordered_map leaves the determinism gate GREEN,
    // including with the mid-run despawns added to exercise the destroy loop.
    // The divergence BUG-0054 actually measured was the CONTACT order, and the
    // sort in dispatchCollisionEvents is what fixed it.
    //
    // These stay ordered anyway, for a weaker but real reason: an
    // unordered_map's order is a function of INSERTION HISTORY and bucket
    // count, while std::map's is a function of the live key set alone. Two runs
    // that reach the same set of bodies by different spawn/despawn paths — a
    // streaming world, or A/B where a despawn lands in a different frame — get
    // the same order from one and not necessarily from the other. Jolt's own
    // Architecture.md asks for a consistent add/remove order for the same
    // reason: BodyID recycling feeds its contact sort.
    //
    // So: prior-art plus a robustness argument, NOT a measurement. Anyone
    // tempted to revert these should know the gate will not stop them.
    //
    // Cost: walked once per tick over tens to hundreds of entries; an RB-tree
    // walk is not measurable against CharacterVirtual::ExtendedUpdate. If it
    // ever is, keep a hash map for LOOKUP and add a sorted vector for
    // ITERATION — but only with a measurement.
    //
    // The LOOKUP-ONLY maps stay hashed: m_bodyToEntity below and m_charState
    // further down are only ever find/erase/operator[], never iterated, so
    // ordering them would buy no determinism and cost a tree descent on every
    // access. m_charState is the one to watch — it is looked up once per
    // character per fixed step, from INSIDE the loop over m_characters, whose
    // order already fixes the sequence.
    std::map<flecs::entity_t, JPH::BodyID>                        m_entityToBody;
    std::unordered_map<JPH::BodyID, flecs::entity_t, BodyIDHash>  m_bodyToEntity;

    // Character controllers
    struct CharState {
        JPH::Vec3 desiredHoriz = JPH::Vec3::sZero();
        float     vertVel      = 0.0f;
        bool      grounded     = false;
        float     gravityScale = 1.0f;
        float     stepHeight   = 0.3f;
    };
    std::map<flecs::entity_t, JPH::Ref<JPH::CharacterVirtual>>           m_characters;
    // Hashed on purpose — see the note on m_entityToBody. This one is never
    // iterated; updateCharacters reaches it as `m_charState[eid]` from inside
    // the walk over m_characters, so the sequence is already determined and
    // ordering this map would only add a tree descent per character per step.
    std::unordered_map<flecs::entity_t, CharState>                       m_charState;

    // ── Collision events (thread-safe queue) ───────────────────────────
    std::mutex                   m_collisionMutex;
    std::vector<CollisionPair>   m_pendingCollisions;

    struct ContactListenerImpl final : public JPH::ContactListener {
        JoltPlugin* owner = nullptr;
        JPH::ValidateResult OnContactValidate(
            const JPH::Body&, const JPH::Body&,
            JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
            const JPH::ContactManifold&, JPH::ContactSettings&) override {
            std::lock_guard lock(owner->m_collisionMutex);
            owner->m_pendingCollisions.push_back({b1.GetID(), b2.GetID(), true});
        }
        void OnContactPersisted(const JPH::Body&, const JPH::Body&,
            const JPH::ContactManifold&, JPH::ContactSettings&) override {}
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            std::lock_guard lock(owner->m_collisionMutex);
            owner->m_pendingCollisions.push_back(
                {pair.GetBody1ID(), pair.GetBody2ID(), false});
        }
    } m_contactListener;

    void flushCollisionEvents(flecs::world& ecs) {
        std::vector<CollisionPair> local;
        { std::lock_guard lock(m_collisionMutex); local = std::move(m_pendingCollisions); }

        // ── SORT: contacts arrive in THREAD-ARRIVAL order ───────────────────
        // OnContactAdded/OnContactRemoved run on Jolt's worker threads and push
        // under m_collisionMutex, so `local`'s order is a race — and that is
        // not an internal detail: it becomes the order of
        // CollisionEvents::entered and ::exited, which scripts iterate, so two
        // identical runs dispatched gameplay callbacks in different orders.
        // tests/determinism_gate_test.cpp reported it as tier=physics A/A'
        // diverging on CollisionEvents (BUG-0054).
        //
        // Sorted HERE, at the source, and deliberately NOT in the hasher: a
        // defensively-sorting digest would have hidden this while gameplay
        // still observed it.
        //
        // The key normalises the pair (min, max) so a contact reported as (a,b)
        // in one run and (b,a) in another lands in the same place, then breaks
        // ties on the enter flag and the raw first id so the order is total.
        std::sort(local.begin(), local.end(),
            [](const CollisionPair& x, const CollisionPair& y) {
                const uint32_t xa = x.a.GetIndexAndSequenceNumber();
                const uint32_t xb = x.b.GetIndexAndSequenceNumber();
                const uint32_t ya = y.a.GetIndexAndSequenceNumber();
                const uint32_t yb = y.b.GetIndexAndSequenceNumber();
                const uint32_t xlo = std::min(xa, xb), xhi = std::max(xa, xb);
                const uint32_t ylo = std::min(ya, yb), yhi = std::max(ya, yb);
                if (xlo != ylo) return xlo < ylo;
                if (xhi != yhi) return xhi < yhi;
                if (x.enter != y.enter) return x.enter < y.enter;
                return xa < ya;
            });

        // Build per-entity event map. ORDERED: the apply loop below runs inside
        // a defer scope, so this map's iteration order IS the order of the
        // flecs command buffer — an unordered_map put a hash-table walk in
        // charge of the sequence of structural changes.
        std::map<flecs::entity_t, CollisionEvents> evMap;
        for (auto& p : local) {
            auto i1 = m_bodyToEntity.find(p.a);
            auto i2 = m_bodyToEntity.find(p.b);
            if (i1==m_bodyToEntity.end()||i2==m_bodyToEntity.end()) continue;
            if (p.enter) {
                evMap[i1->second].entered.push_back(i2->second);
                evMap[i2->second].entered.push_back(i1->second);
            } else {
                evMap[i1->second].exited.push_back(i2->second);
                evMap[i2->second].exited.push_back(i1->second);
            }
        }

        // Defer all structural ECS changes — flecs locks archetype tables
        // during each(), calling remove/set inside would crash (LOCKED_STORAGE).
        ecs.defer_begin();
        // Apply new events
        for (auto& [eid, ev] : evMap) {
            flecs::entity e = ecs.entity(eid);
            if (e.is_alive()) e.set<CollisionEvents>(ev);
        }
        // Remove stale CollisionEvents from last frame
        ecs.query_builder<CollisionEvents>().build()
            .each([&](flecs::entity e, CollisionEvents&) {
                if (evMap.find(e.id()) == evMap.end())
                    e.remove<CollisionEvents>();
            });
        ecs.defer_end(); // flush deferred structural changes
    }

    void spawnBody(flecs::entity e, const Transform& t, const RigidBody& rb) {
        // Apply entity scale to shape dimensions so physics matches visual size.
        // Without this, a plane scaled (10,1,10) would have a 1x1x1 collision box.
        const float sx = std::max(t.scale.x, 0.001f);
        const float sy = std::max(t.scale.y, 0.001f);
        const float sz = std::max(t.scale.z, 0.001f);
        const float maxS = std::max({sx, sy, sz});

        JPH::ShapeRefC shape;
        switch (rb.shape) {
        case PhysicsShape::Sphere:
            shape = JPH::SphereShapeSettings(rb.radius * maxS).Create().Get();
            break;
        case PhysicsShape::Capsule:
            shape = JPH::CapsuleShapeSettings(
                rb.halfHeight * sy, rb.radius * std::max(sx, sz)).Create().Get();
            break;
        default: // Box
            shape = JPH::BoxShapeSettings(
                JPH::Vec3(rb.halfExtent.x * sx,
                          rb.halfExtent.y * sy,
                          rb.halfExtent.z * sz)).Create().Get();
            break;
        }

        JPH::EMotionType motionType;
        JPH::ObjectLayer layer;
        switch (rb.bodyType) {
        case PhysicsBodyType::Static:
            motionType = JPH::EMotionType::Static;
            layer      = PhysLayers::STATIC; break;
        case PhysicsBodyType::Kinematic:
            motionType = JPH::EMotionType::Kinematic;
            layer      = PhysLayers::DYNAMIC; break;
        default:
            motionType = JPH::EMotionType::Dynamic;
            layer      = PhysLayers::DYNAMIC; break;
        }

        // Use world transform — local t is relative to parent if parented
        float wm[16]; getWorldMatrix(e, wm);
        bx::Vec3       wp  = {wm[12], wm[13], wm[14]};
        bx::Quaternion wr  = quatFromMatrix(wm);
        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(wp.x, wp.y, wp.z),
            JPH::Quat(wr.x, wr.y, wr.z, wr.w),
            motionType, layer
        );
        settings.mRestitution   = rb.restitution;
        settings.mFriction      = rb.friction;
        settings.mGravityFactor =
            (motionType == JPH::EMotionType::Dynamic && rb.useGravity) ? 1.0f : 0.0f;
        if (motionType == JPH::EMotionType::Dynamic) {
            settings.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rb.mass;
        }

        JPH::BodyID bid = m_physics->GetBodyInterface().CreateAndAddBody(
            settings,
            motionType == JPH::EMotionType::Static
                ? JPH::EActivation::DontActivate
                : JPH::EActivation::Activate);

        if (!bid.IsInvalid()) {
            m_entityToBody[e.id()] = bid;
            m_bodyToEntity[bid]    = e.id();
        } else {
            LOG_WARN("Physics", "Body creation failed for entity %llu", (uint64_t)e.id());
        }
    }

    void spawnCharacter(flecs::entity e, const Transform&, const CharacterController& cc) {
        float radius  = std::max(0.05f, cc.radius);
        float halfCyl = std::max(0.0f, cc.height * 0.5f - radius);
        JPH::ShapeRefC capsule = JPH::CapsuleShapeSettings(halfCyl, radius).Create().Get();
        JPH::ShapeRefC shape = JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0, cc.height * 0.5f, 0), JPH::Quat::sIdentity(), capsule).Create().Get();

        float wm[16]; getWorldMatrix(e, wm);
        JPH::CharacterVirtualSettings settings;
        settings.mShape         = shape;
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(cc.maxSlopeDeg);
        settings.mMass          = cc.mass;

        // The entity's transform is the RENDER origin; the character reference
        // is the FEET, footOffset below it (so a model whose origin isn't at
        // the feet spawns with its feet on the ground, not floating/sunk).
        JPH::Ref<JPH::CharacterVirtual> ch = new JPH::CharacterVirtual(
            &settings, JPH::RVec3(wm[12], wm[13] - cc.footOffset, wm[14]),
            JPH::Quat::sIdentity(), m_physics.get());

        m_characters[e.id()] = ch;
        CharState st; st.gravityScale = cc.gravityScale; st.stepHeight = cc.stepHeight;
        m_charState[e.id()] = st;
    }

    void updateCharacters(float dt) {
        if (m_characters.empty()) return;
        JPH::Vec3 gravity = m_physics->GetGravity();
        JPH::DefaultBroadPhaseLayerFilter bpFilter =
            m_physics->GetDefaultBroadPhaseLayerFilter(PhysLayers::DYNAMIC);
        JPH::DefaultObjectLayerFilter objFilter =
            m_physics->GetDefaultLayerFilter(PhysLayers::DYNAMIC);
        JPH::BodyFilter  bodyFilter;
        JPH::ShapeFilter shapeFilter;

        for (auto& [eid, ch] : m_characters) {
            CharState& st = m_charState[eid];
            bool grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            if (grounded && st.vertVel < 0.0f) st.vertVel = 0.0f;
            st.vertVel += gravity.GetY() * st.gravityScale * dt;

            ch->SetLinearVelocity(st.desiredHoriz + JPH::Vec3(0, st.vertVel, 0));

            JPH::CharacterVirtual::ExtendedUpdateSettings up;
            up.mWalkStairsStepUp = JPH::Vec3(0, st.stepHeight, 0);
            ch->ExtendedUpdate(dt, gravity * st.gravityScale, up,
                bpFilter, objFilter, bodyFilter, shapeFilter, *m_tempAllocator);

            st.grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        }
    }

    void writeBackCharacters(flecs::world& ecs) {
        ecs.query_builder<Transform, CharacterController>().build()
            .each([&](flecs::entity e, Transform& t, CharacterController& cc) {
                auto it = m_characters.find(e.id());
                if (it == m_characters.end()) return;
                JPH::RVec3 p = it->second->GetPosition();
                // Lift the render origin back above the feet by footOffset.
                const float rx = (float)p.GetX();
                const float ry = (float)p.GetY() + cc.footOffset;
                const float rz = (float)p.GetZ();
                flecs::entity par = e.target(flecs::ChildOf);
                if (par && par.is_alive() && par.has<Transform>()) {
                    float world[16]; bx::mtxIdentity(world);
                    world[12]=rx; world[13]=ry; world[14]=rz;
                    float parentWorld[16]; getWorldMatrix(par, parentWorld);
                    float parentInv[16];   safeInvert(parentInv, parentWorld);
                    float local[16];       bx::mtxMul(local, world, parentInv);
                    t.position = {local[12], local[13], local[14]};
                } else {
                    t.position = {rx, ry, rz};
                }
                auto sit = m_charState.find(e.id());
                cc.grounded = (sit != m_charState.end()) && sit->second.grounded;
            });
    }

    // ── ECS -> physics, for the two things physics does NOT own ─────────────
    //
    // BUG-0058 — A KINEMATIC BODY COULD NOT BE MOVED AT ALL. Its Jolt pose is
    // authoritative and nothing ever drove it: no MoveKinematic, no
    // SetPositionAndRotation, nowhere in the tree. Meanwhile
    // writeBackTransforms copies that unmoving pose back onto Transform every
    // step, so a gameplay write was not merely ignored — it was erased.
    // Moving platforms, lifts, doors and swinging hazards were all unbuildable,
    // silently. Kinematic is now DRIVEN BY THE ECS TRANSFORM, which is the
    // natural reading: gameplay says where the platform should be, physics
    // carries it there with a real velocity so dynamic bodies riding it are
    // pushed correctly. Setting the pose directly would teleport it and drop
    // that velocity, which is why this is MoveKinematic and not SetPosition.
    //
    // BUG-0059 — A CHARACTER'S COLLISION SHAPE NEVER ROTATED. CharacterVirtual
    // holds its own mRotation (CharacterVirtual.h:621) feeding GetWorldTransform,
    // GetTransformedShape and the shape offset; spawnCharacter passed
    // Quat::sIdentity() and nothing in the engine has ever called SetRotation.
    // The visual turned and the capsule did not. Harmless ONLY because the
    // shape is a centred capsule — radially symmetric about the axis anything
    // upright rotates around — and wrong the moment a shape is offset, or not a
    // capsule, or a character pitches. Rotation stays GAMEPLAY-owned; it is
    // pushed in, never read back, so no kit changes.
    void pushEcsToPhysics(flecs::world& ecs) {
        auto& bi = m_physics->GetBodyInterface();
        m_bodySyncQ.each([&](flecs::entity e, const Transform&,
                             const RigidBody& rb) {
            if (rb.bodyType != PhysicsBodyType::Kinematic) return;
            auto it = m_entityToBody.find(e.id());
            if (it == m_entityToBody.end()) return;
            // The WORLD pose: Transform is local, and the body lives at world
            // root — the same conversion spawnBody does when it creates it.
            float wm[16]; getWorldMatrix(e, wm);
            const bx::Quaternion wr = quatFromMatrix(wm);
            bi.MoveKinematic(it->second,
                             JPH::RVec3(wm[12], wm[13], wm[14]),
                             JPH::Quat(wr.x, wr.y, wr.z, wr.w), kFixedDt);
        });

        m_charSyncQ.each([&](flecs::entity e, const Transform&,
                             const CharacterController&) {
            auto it = m_characters.find(e.id());
            if (it == m_characters.end()) return;
            float wm[16]; getWorldMatrix(e, wm);
            const bx::Quaternion wr = quatFromMatrix(wm);
            it->second->SetRotation(JPH::Quat(wr.x, wr.y, wr.z, wr.w));
        });
    }

    void writeBackTransforms(flecs::world& ecs) {
        auto& bi = m_physics->GetBodyInterface();
        ecs.query_builder<Transform, const RigidBody>().build()
            .each([&](flecs::entity e, Transform& t, const RigidBody& rb) {
                // ── ONLY DYNAMIC BODIES ARE WRITTEN BACK ───────────────────
                // Static never moves. KINEMATIC is now GAMEPLAY-OWNED — it is
                // driven from the ECS in pushEcsToPhysics — and writing the
                // resulting pose back would be a round trip through a velocity
                // integration that does not land exactly on its target, so the
                // Transform gameplay just set would drift by a few ULPs every
                // step. Physics owns a body's pose exactly when physics decides
                // it, which for a kinematic body it does not.
                if (rb.bodyType != PhysicsBodyType::Dynamic) return;
                auto it = m_entityToBody.find(e.id());
                if (it == m_entityToBody.end()) return;
                JPH::Vec3 p = bi.GetPosition(it->second);
                JPH::Quat q = bi.GetRotation(it->second);
                // Jolt returns a WORLD pose, but Transform is LOCAL and
                // rendering re-applies the parent via getWorldMatrix. For a
                // parented body, convert world -> local so the parent is not
                // applied twice (which caused drift/jumps).
                flecs::entity par = e.target(flecs::ChildOf);
                if (par && par.is_alive() && par.has<Transform>()) {
                    float world[16];
                    bx::mtxFromQuaternion(world, bx::Quaternion{q.GetX(),q.GetY(),q.GetZ(),q.GetW()});
                    world[12]=p.GetX(); world[13]=p.GetY(); world[14]=p.GetZ();
                    float parentWorld[16]; getWorldMatrix(par, parentWorld);
                    float parentInv[16];   safeInvert(parentInv, parentWorld);
                    float local[16];       bx::mtxMul(local, world, parentInv);
                    bx::Vec3 lp{0,0,0}; bx::Quaternion lr{0,0,0,1}; bx::Vec3 ls{1,1,1};
                    decomposeMatrix(local, lp, lr, ls);
                    t.position = lp; t.rotation = lr; // scale stays local
                } else {
                    t.position = {p.GetX(), p.GetY(), p.GetZ()};
                    t.rotation = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
                }
            });
    }
};
