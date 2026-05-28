#pragma once
#include <memory>
#include <unordered_map>
#include <thread>
#include <imgui.h>

#include "engine/plugin.h"
#include "engine/logger.h"
#include "components/collision_events.h"
#include <mutex>
#include <unordered_map>
#include "components/rigid_body.h"
#include "core/transform.h"

// Jolt headers — Jolt.h must be first
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

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
class JoltPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Physics"; }
    const char* version() const override { return "0.1.0-jolt5"; }

    void onAttach(EngineContext&) override {
        JPH::RegisterDefaultAllocator();
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
        m_jobSystem     = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            std::max(1, (int)std::thread::hardware_concurrency() - 1));

        m_physics = std::make_unique<JPH::PhysicsSystem>();
        m_physics->Init(4096, 0, 4096, 4096,
            m_bpInterface, m_objVsBP, m_objPairFilter);
        m_physics->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

        ecs.query_builder<const Transform, const RigidBody>().build()
            .each([this](flecs::entity e, const Transform& t, const RigidBody& rb) {
                spawnBody(e, t, rb);
            });

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
        m_physics.reset();
        m_jobSystem.reset();
        m_tempAllocator.reset();
        m_accumulator = 0.0f;
        LOG_INFO("Physics", "Simulation stop");
    }

    void onUpdate(flecs::world& ecs, float dt) override {
        if (!m_physics) return;
        m_accumulator += dt;
        int steps = 0;
        while (m_accumulator >= kFixedDt && steps < 4) {
            m_physics->Update(kFixedDt, 1,
                m_tempAllocator.get(), m_jobSystem.get());
            m_accumulator -= kFixedDt;
            ++steps;
        }
        writeBackTransforms(ecs);
        flushCollisionEvents(ecs);
    }

    void onEditorUI() override {
        if (m_physics) {
            ImGui::TextColored({0.3f,1.0f,0.4f,1.0f}, "Jolt %d.%d.%d  ACTIVE",
                JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH);
            ImGui::Text("Bodies:   %d", (int)m_entityToBody.size());
            ImGui::Text("Fixed dt: %.0f Hz", 1.0f / kFixedDt);
        } else {
            ImGui::TextColored({0.3f,1.0f,0.4f,1.0f}, "Jolt %d.%d.%d  standby",
                JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH);
            ImGui::TextDisabled("Enter play mode to simulate");
        }
        ImGui::TextDisabled("Gravity: (0, -9.81, 0)");
    }

private:
    static constexpr float kFixedDt = 1.0f / 60.0f;

    BPLayerInterfaceImpl m_bpInterface;
    ObjVsBPFilter        m_objVsBP;
    ObjLayerPairFilter   m_objPairFilter;

    std::unique_ptr<JPH::TempAllocatorImpl>   m_tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;
    std::unique_ptr<JPH::PhysicsSystem>       m_physics;
    float                                      m_accumulator = 0.0f;

    std::unordered_map<flecs::entity_t, JPH::BodyID>              m_entityToBody;
    std::unordered_map<JPH::BodyID, flecs::entity_t, BodyIDHash>  m_bodyToEntity;

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

        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(t.position.x, t.position.y, t.position.z),
            JPH::Quat(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w),
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

    void writeBackTransforms(flecs::world& ecs) {
        auto& bi = m_physics->GetBodyInterface();
        ecs.query_builder<Transform, const RigidBody>().build()
            .each([&](flecs::entity e, Transform& t, const RigidBody& rb) {
                if (rb.bodyType == PhysicsBodyType::Static) return;
                auto it = m_entityToBody.find(e.id());
                if (it == m_entityToBody.end()) return;
                JPH::Vec3 p = bi.GetPosition(it->second);
                JPH::Quat q = bi.GetRotation(it->second);
                t.position = {p.GetX(), p.GetY(), p.GetZ()};
                t.rotation = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
            });
    }
};
