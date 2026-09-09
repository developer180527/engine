// ── physics_authority_test — who is allowed to move a physics entity ────────
//
// Stage 3a of the command architecture: three defects that are user-visible
// correctness and need none of the authority watcher.
//
//   BUG-0057  A gameplay write to a physics body's Transform is SILENTLY
//             DISCARDED — writeBackTransforms overwrites it from the body at
//             the end of every step. The fix is not to let the write through
//             (two authorities over one pose is the defect this architecture
//             removes) but to give the intent a name: Teleport, which moves the
//             body, the Transform and the interpolation history together and
//             clears velocity.
//   BUG-0058  A KINEMATIC BODY COULD NOT BE MOVED AT ALL. Nothing in the tree
//             ever drove its pose — no MoveKinematic, no SetPositionAndRotation
//             — while the write-back copied that unmoving pose back over the
//             Transform. Moving platforms, lifts and doors were unbuildable,
//             silently.
//   BUG-0059  A CHARACTER'S COLLISION SHAPE NEVER ROTATED. spawnCharacter
//             passed Quat::sIdentity() and nothing has ever called SetRotation.
//
// ── WHY BUG-0059 NEEDS A DIAGNOSTIC ACCESSOR AND THE OTHERS DO NOT ──────────
// The controller's shape is a centred capsule — radially symmetric about the
// axis anything upright turns around — so a rotation that never arrives changes
// no collision result a test can observe. That is precisely why it survived,
// and a test that could only check observable collisions would keep missing it.
// So this reads the rotation the CharacterVirtual actually holds.
#include <cmath>
#include <cstdio>
#include <memory>

#include <flecs.h>

#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/prev_transform.h"
#include "components/rigid_body.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "project/project_context.h"
#include "assets/importers/importer_registry.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "runtime/jobs/jobs.h"
#include "runtime/runtime_context.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static constexpr float kDt = 1.0f / 60.0f;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("physics_authority_test — who may move a physics entity\n");
    jobs::init();

    flecs::world w;
    AssetRegistry assets; TextureRegistry tex; MaterialRegistry mat;
    ProjectContext proj; ImporterRegistry imp;
    RuntimeContext ctx{ w, assets, tex, mat, proj, imp };

    // Static floor, top at y = 0.
    RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static;
    floor.halfExtent = { 50.0f, 0.5f, 50.0f };
    w.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}})
              .set<RigidBody>(floor);

    // A kinematic platform, a dynamic box, and a character.
    RigidBody plat{}; plat.bodyType = PhysicsBodyType::Kinematic;
    plat.halfExtent = { 1.0f, 0.25f, 1.0f };
    flecs::entity platform =
        w.entity().set<Transform>({{0.f, 1.f, 0.f},{0,0,0,1},{1,1,1}})
                  .set<RigidBody>(plat);

    RigidBody dyn{}; dyn.bodyType = PhysicsBodyType::Dynamic;
    dyn.halfExtent = { 0.5f, 0.5f, 0.5f }; dyn.mass = 1.0f;
    flecs::entity box =
        w.entity().set<Transform>({{20.f, 4.f, 0.f},{0,0,0,1},{1,1,1}})
                  .set<RigidBody>(dyn);

    CharacterController cc{};
    // NON-ZERO on purpose. The reference point of a CharacterVirtual is the
    // FEET, footOffset below the render origin, and the default is 0 — so a
    // character built with the default cannot distinguish a teleport that
    // handles the offset from one that ignores it. Section 5 depends on this.
    cc.footOffset = 0.9f;
    flecs::entity hero =
        w.entity().set<Transform>({{-10.f, 1.f, 0.f},{0,0,0,1},{1,1,1}})
                  .set<CharacterController>(cc);

    JoltPlugin jolt;
    jolt.onAttach(ctx);
    jolt.onSimulationStart(w);

    // A downward ray from well above `x`, used to ask where a body ACTUALLY is
    // rather than where the ECS says it is — the two disagreeing is the whole
    // subject of this file.
    auto hitBelow = [&](float x, float z) {
        return jolt.raycast(x, 8.0f, z, 0.0f, -1.0f, 0.0f, 20.0f);
    };

    // ── 1. A kinematic body can be moved (BUG-0058) ────────────────────────
    {
        std::printf("\n-- 1. kinematic reachability --\n");
        RaycastHit before = hitBelow(5.0f, 0.0f);
        CHECK(before.hit && before.entity != (uint64_t)platform.id(),
              "the platform does not start at x=5 (the floor is what is there)");

        platform.get_mut<Transform>().position = { 5.0f, 1.0f, 0.0f };
        for (int i = 0; i < 8; ++i) jolt.onPhysicsStep(w, kDt);

        RaycastHit after = hitBelow(5.0f, 0.0f);
        CHECK(after.hit && after.entity == (uint64_t)platform.id(),
              "writing Transform MOVED the kinematic body — before this it was "
              "unreachable: nothing in the tree ever drove a kinematic pose");

        // The other half of the bug, and the more insidious one: the write-back
        // copied the body's unmoving pose back over the Transform, so gameplay's
        // write was not merely ignored, it was erased.
        // EXACT equality, and the exactness is the assertion. An approximate
        // one passes with the write-back restored: the round trip through
        // MoveKinematic's velocity integration lands on 4.99999952, not 5, so a
        // 1e-3 tolerance sees a bug-for-bug revert as correct. Gameplay owns
        // this value; it must come back bit-identical or physics is still the
        // second writer, just a quieter one.
        const Transform& t = platform.get<Transform>();
        CHECK(t.position.x == 5.0f,
              "and the Transform came back BIT-IDENTICAL (%.9g) — kinematic is "
              "gameplay-owned, so nothing may round-trip it through physics",
              (double)t.position.x);
    }

    // ── 2. A character's collision shape rotates with it (BUG-0059) ────────
    {
        std::printf("\n-- 2. character rotation reaches the controller --\n");
        float q[4] = { -1, -1, -1, -1 };
        CHECK(jolt.characterRotation(hero.id(), q), "the character exists");
        CHECK(std::fabs(q[3] - 1.0f) < 1e-4f,
              "it starts at identity, as spawnCharacter creates it");

        // 90 degrees about Y: quat (0, sin45, 0, cos45).
        const float s = std::sin(0.7853981634f), c = std::cos(0.7853981634f);
        hero.get_mut<Transform>().rotation = { 0.0f, s, 0.0f, c };
        jolt.onPhysicsStep(w, kDt);

        CHECK(jolt.characterRotation(hero.id(), q) &&
              std::fabs(q[1] - s) < 1e-3f && std::fabs(q[3] - c) < 1e-3f,
              "the ECS rotation reached CharacterVirtual (%.4f, %.4f) — the "
              "visual used to turn while the capsule stayed at identity",
              (double)q[1], (double)q[3]);

        // Rotation is pushed IN and never read back: it stays gameplay-owned,
        // which is what keeps existing kits working unchanged.
        const Transform& t = hero.get<Transform>();
        CHECK(std::fabs(t.rotation.y - s) < 1e-4f,
              "and the ECS rotation is still gameplay's — not written back");
    }

    // ── 3. Teleport moves a dynamic body, and clears its momentum ──────────
    {
        std::printf("\n-- 3. teleport --\n");
        // Let it fall so it has real velocity to discard.
        for (int i = 0; i < 10; ++i) jolt.onPhysicsStep(w, kDt);
        float vx = 0, vy = 0, vz = 0;
        jolt.getVelocity(w, box.id(), vx, vy, vz);
        CHECK(std::fabs(vy) > 0.5f,
              "the box is falling before the teleport (vy=%.3f)", (double)vy);

        CHECK(jolt.teleport(w, box.id(), -20.0f, 3.0f, 0.0f, nullptr),
              "teleport reports it moved a body it owns");
        jolt.getVelocity(w, box.id(), vx, vy, vz);
        CHECK(std::fabs(vx) + std::fabs(vy) + std::fabs(vz) < 1e-3f,
              "velocity is CLEARED — a body that reappears across the map "
              "carrying its old momentum is the classic teleport bug");

        RaycastHit at = hitBelow(-20.0f, 0.0f);
        CHECK(at.hit && at.entity == (uint64_t)box.id(),
              "and the body is actually at the destination");

        // The point of BUG-0057: unlike a Transform write, this survives the
        // step's write-back, because the body is where the command put it.
        jolt.onPhysicsStep(w, kDt);
        const Transform& t = box.get<Transform>();
        CHECK(std::fabs(t.position.x + 20.0f) < 0.1f,
              "the Transform agrees after a step (x=%.3f) — a plain write here "
              "would have been silently overwritten from the body",
              (double)t.position.x);
    }

    // ── 4. The write a teleport EXISTS to replace, still refused ───────────
    // Not a regression to fix — a property to keep. Physics owning a dynamic
    // body's pose is what makes the simulation single-authority, so this
    // records the behaviour rather than lamenting it, and is what the stage-3b
    // watcher will report on instead of silently dropping.
    {
        std::printf("\n-- 4. a raw Transform write on a dynamic body --\n");
        box.get_mut<Transform>().position = { 40.0f, 9.0f, 0.0f };
        jolt.onPhysicsStep(w, kDt);
        const Transform& t = box.get<Transform>();
        CHECK(std::fabs(t.position.x - 40.0f) > 1.0f,
              "is still discarded (x=%.3f, not 40) — DELIBERATE, and the "
              "reason teleport exists; the watcher in stage 3b makes it loud "
              "rather than silent",
              (double)t.position.x);
    }

    // ── 5. Teleporting a character lands its FEET, not its origin ──────────
    // The character's reference point is footOffset below the render origin.
    // A teleport that ignored the offset would sink or float the character by
    // that much, every time — a slow drift for anything that teleports often.
    {
        std::printf("\n-- 5. teleporting a character --\n");
        CHECK(jolt.teleport(w, hero.id(), 12.0f, 2.0f, 0.0f, nullptr),
              "teleport reports it moved the character");
        jolt.onPhysicsStep(w, kDt);
        const Transform& t = hero.get<Transform>();
        CHECK(std::fabs(t.position.x - 12.0f) < 0.1f,
              "the character is at the destination in x (%.3f)",
              (double)t.position.x);
        CHECK(std::fabs(t.position.y - 2.0f) < 0.1f,
              "and at the destination in y (%.3f) — the write-back re-applies "
              "footOffset, so a teleport that did not subtract it would land "
              "the character one offset high",
              (double)t.position.y);
    }

    jolt.onSimulationStop();
    jolt.onDetach();
    jobs::shutdown();

    if (g_failures) {
        std::printf("\nphysics_authority_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nphysics_authority_test: ALL PASS\n");
    return 0;
}
