// ── sim_classification — which components ARE the simulation ────────────────
//
// The answer to "what is simulation state in this engine", as data rather than
// as something you reconstruct by reading systems. Called from the same place
// MetaRegistry::registerAll is, and deliberately NOT part of it: reflection
// (what the inspector and the scene serializer can see) and reproducibility
// (what a divergence would be observable in) are different questions with
// different answers, and merging them makes both wrong.
//
// ── THE GOVERNING RULE ──────────────────────────────────────────────────────
//   ** Hash every field that can influence FUTURE simulation behaviour. **
//
// Not "every field", and not "the fields that happen to be serialized".
// Animator::fade is a crossfade DURATION — it changes the sampled pose over
// subsequent ticks, so it is in, even though a first pass at this list left it
// out. Camera::fov changes what you see and nothing the simulation does, so
// Camera is out entirely.
//
// Each exemption carries a written reason. An exemption without one is
// indistinguishable from an oversight, and the registry refuses it.
#include "runtime/sim_classification.h"

#include <bx/math.h>

#include "components/animator.h"
#include "components/camera.h"
#include "components/camera_look.h"
#include "components/character_controller.h"
#include "components/collision_events.h"
#include "components/entity_id.h"
#include "components/light.h"
#include "components/lod_mesh.h"
#include "components/mesh_renderer.h"
#include "components/name.h"
#include "components/prev_transform.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/skinned_mesh.h"
#include "components/spinner.h"
#include "core/transform.h"
#include "scene/reflected_serde.h"          // reflected::ReflectedPending

namespace simhash {
namespace {

void hashVec3(const bx::Vec3& v, Digest& d) { d.f32(v.x); d.f32(v.y); d.f32(v.z); }
void hashQuat(const bx::Quaternion& q, Digest& d) {
    d.f32(q.x); d.f32(q.y); d.f32(q.z); d.f32(q.w);
}

void hTransform(flecs::entity, const void* p, Digest& d) {
    const auto& t = *static_cast<const Transform*>(p);
    hashVec3(t.position, d); hashQuat(t.rotation, d); hashVec3(t.scale, d);
}

void hRigidBody(flecs::entity, const void* p, Digest& d) {
    const auto& r = *static_cast<const RigidBody*>(p);
    d.i32(static_cast<int32_t>(r.bodyType));
    d.i32(static_cast<int32_t>(r.shape));
    d.f32(r.mass); d.f32(r.restitution); d.f32(r.friction);
    d.boolean(r.useGravity);
    hashVec3(r.halfExtent, d);
    d.f32(r.radius); d.f32(r.halfHeight);
}

void hCharacterController(flecs::entity, const void* p, Digest& d) {
    const auto& c = *static_cast<const CharacterController*>(p);
    d.f32(c.radius); d.f32(c.height); d.f32(c.maxSlopeDeg); d.f32(c.stepHeight);
    d.f32(c.mass); d.f32(c.gravityScale); d.f32(c.footOffset);
    d.boolean(c.grounded);           // per-tick state a script can read
}

// STORED ORDER, deliberately unsorted. Contacts are pushed from Jolt worker
// threads under a mutex, so their order is a thread race — and that order is
// what scripts iterate. Sorting here would make the gate blind to exactly the
// nondeterminism that is known to exist while gameplay still observed it. The
// fix belongs at the source (JoltPlugin::flushCollisionEvents), not here.
void hCollisionEvents(flecs::entity, const void* p, Digest& d) {
    const auto& c = *static_cast<const CollisionEvents*>(p);
    d.u64(c.entered.size());
    for (flecs::entity_t e : c.entered) d.u64(e);
    d.u64(c.exited.size());
    for (flecs::entity_t e : c.exited)  d.u64(e);
}

// `clip` is excluded as a session-local handle whose id carries no meaning
// across runs; WHICH clip is playing is fully determined by clipPath+clipIndex,
// which are hashed. `fade` IS included — it is the crossfade duration and
// changes the pose sampled on subsequent ticks.
void hAnimator(flecs::entity, const void* p, Digest& d) {
    const auto& a = *static_cast<const Animator*>(p);
    d.str(a.clipPath);
    d.i32(a.clipIndex);
    d.f32(a.time); d.f32(a.speed); d.f32(a.fade);
    d.boolean(a.playing); d.boolean(a.looping);
}

// Name IS simulation state, and this was challenged in review. World.find(name)
// is exposed to Lua (runtime/scripting/lua_bindings.h:299 -> ScriptHost::find),
// so renaming an entity changes which entity a script resolves. Verified, not
// assumed.
void hName(flecs::entity, const void* p, Digest& d) {
    d.str(static_cast<const Name*>(p)->value);
}

// instanceId is backend-owned (a Lua registry index) and carries no meaning
// across runs. scriptPath selects the behaviour; started gates onStart.
void hScriptComponent(flecs::entity, const void* p, Digest& d) {
    const auto& s = *static_cast<const ScriptComponent*>(p);
    d.str(s.scriptPath);
    d.boolean(s.started);
}

void hEntityId(flecs::entity, const void* p, Digest& d) {
    d.u64(static_cast<const EntityId*>(p)->value);
}

void hSpinner(flecs::entity, const void* p, Digest& d) {
    const auto& s = *static_cast<const Spinner*>(p);
    d.f32(s.speedYaw); d.f32(s.speedPitch);
}

}  // namespace

void registerClassification(flecs::world& w) {
    if (w.template component<Transform>().template has<SimState>()) return;  // idempotent

    // ── Simulation state ────────────────────────────────────────────────────
    declare<Transform>(w,           hTransform);
    declare<RigidBody>(w,           hRigidBody);
    declare<CharacterController>(w, hCharacterController);
    declare<CollisionEvents>(w,     hCollisionEvents);
    declare<Animator>(w,            hAnimator);
    declare<Name>(w,                hName);
    declare<ScriptComponent>(w,     hScriptComponent);
    declare<EntityId>(w,            hEntityId);
    declare<Spinner>(w,             hSpinner);

    // ── Not simulation state, each with its reason ──────────────────────────
    exempt<PrevTransform>(w,
        "render interpolation snapshot — derived from Transform every tick, "
        "never read by the simulation");
    exempt<SkinnedMesh>(w,
        "paletteSlot is an allocation from a mutex-guarded global pool acquired "
        "on job workers (animation/skin_palette.h), so it is nondeterministic "
        "BY CONSTRUCTION and is a render resource, not simulation state");
    exempt<Camera>(w,       "presentation — projection and framing, read by the renderer");
    // Written at RENDER RATE by a look controller and composed by
    // PrimaryCameraFinder. Exempt because it is presentation: it decides what
    // is drawn and nothing in the simulation reads it. This exemption is the
    // whole point of the component — before it, the same aim was written into
    // Transform.rotation, which IS hashed, making a render-rate write reach
    // simulation state (BUG-0053's class). See components/camera_look.h for
    // what this does NOT fix.
    exempt<CameraLook>(w,   "presentation — where the camera is aimed, latched "
                            "at render rate so look does not lag the frame");
    exempt<Light>(w,        "presentation — shading parameters");
    exempt<LodMesh>(w,      "presentation — which detail level to draw");
    exempt<MeshRenderer>(w, "presentation — which mesh and material to draw");
    exempt<reflected::ReflectedPending>(w,
        "load-time bookkeeping — blobs awaiting a component type that has not "
        "registered yet; consumed by reflected::applyPending, never simulated");
}

}  // namespace simhash
