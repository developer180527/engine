#pragma once
#include <flecs.h>
#include <bx/math.h>
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/spinner.h"
#include "components/rigid_body.h"
#include "engine/logger.h"

// ── MetaRegistry ───────────────────────────────────────────────────────────
// Registers all engine component types with the flecs meta addon.
// One registration site drives everything downstream:
//   - flecs world inspector / REST API
//   - future Lua FFI typedef generation
//   - future Blueprint node pin types
//   - future editor inspector auto-generation
//
// Call registerAll() once after flecs::world is created, before any
// entities are spawned. Safe to call on both editor and game worlds.
namespace MetaRegistry {

inline void registerAll(flecs::world& ecs) {
    // ── Primitive math types ───────────────────────────────────────────────
    // Register bx vector/quaternion types so component members that use
    // them are fully described in the meta schema.
    ecs.component<bx::Vec3>()
        .member<float>("x")
        .member<float>("y")
        .member<float>("z");

    ecs.component<bx::Quaternion>()
        .member<float>("x")
        .member<float>("y")
        .member<float>("z")
        .member<float>("w");

    // ── Core components ────────────────────────────────────────────────────
    ecs.component<Transform>()
        .member<bx::Vec3>("position")
        .member<bx::Quaternion>("rotation")
        .member<bx::Vec3>("scale");

    // Name: std::string is not trivially meta-serializable.
    // Registered by name only — field-level access goes through the
    // inspector panel directly until we add an opaque string descriptor.
    ecs.component<Name>();

    // MeshRenderer: handle IDs are opaque uint32 — register by name.
    // Field-level meta added when we build the Blueprint node system.
    ecs.component<MeshRenderer>();

    // Camera: register inspectable fields
    ecs.component<Camera>()
        .member<bool>("isPrimary")
        .member<float>("fov")
        .member<float>("orthoSize")
        .member<float>("nearPlane")
        .member<float>("farPlane");

    // Spinner: editor-only, excluded from game world snapshots
    ecs.component<Spinner>()
        .member<float>("speedYaw")
        .member<float>("speedPitch");

    // RigidBody: physics simulation component
    ecs.component<RigidBody>()
        .member<float>("mass")
        .member<float>("restitution")
        .member<float>("friction")
        .member<bool>("useGravity");

    LOG_INFO("Meta", "Component schemas registered — %d types", 8);
}

} // namespace MetaRegistry
