#pragma once
#include <memory>
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
        m_jobSystem.reset();
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
    std::unordered_map<flecs::entity_t, JPH::BodyID>              m_entityToBody;
    std::unordered_map<JPH::BodyID, flecs::entity_t, BodyIDHash>  m_bodyToEntity;

    // Character controllers
    struct CharState {
        JPH::Vec3 desiredHoriz = JPH::Vec3::sZero();
        float     vertVel      = 0.0f;
        bool      grounded     = false;
        float     gravityScale = 1.0f;
        float     stepHeight   = 0.3f;
    };
    std::unordered_map<flecs::entity_t, JPH::Ref<JPH::CharacterVirtual>> m_characters;
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

        // Build per-entity event map
        std::unordered_map<flecs::entity_t, CollisionEvents> evMap;
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

        JPH::Ref<JPH::CharacterVirtual> ch = new JPH::CharacterVirtual(
            &settings, JPH::RVec3(wm[12], wm[13], wm[14]),
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
                flecs::entity par = e.target(flecs::ChildOf);
                if (par && par.is_alive() && par.has<Transform>()) {
                    float world[16]; bx::mtxIdentity(world);
                    world[12]=(float)p.GetX(); world[13]=(float)p.GetY(); world[14]=(float)p.GetZ();
                    float parentWorld[16]; getWorldMatrix(par, parentWorld);
                    float parentInv[16];   safeInvert(parentInv, parentWorld);
                    float local[16];       bx::mtxMul(local, world, parentInv);
                    t.position = {local[12], local[13], local[14]};
                } else {
                    t.position = {(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()};
                }
                auto sit = m_charState.find(e.id());
                cc.grounded = (sit != m_charState.end()) && sit->second.grounded;
            });
    }

    void writeBackTransforms(flecs::world& ecs) {
        auto& bi = m_physics->GetBodyInterface();
        ecs.query_builder<Transform, const RigidBody>().build()
            .each([&](flecs::entity e, Transform& t, const RigidBody& rb) {
                if (rb.bodyType == PhysicsBodyType::Static) return;
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
