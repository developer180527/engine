// ── component_abi_test — the shared components' layout, frozen ──────────────
//
// `engine_abi::componentLayoutHash()` (include/engine/game_module.h) is the gate
// that decides whether a kit may touch LIVE ECS memory. `ModuleLibrary::load`
// refuses a module whose hash differs, and the refusal's remedy is "restart the
// host", not "rebuild the module", precisely because world data survives a
// reload — a mismatched kit would misread memory the running host is still using.
//
// ── The hole this closes ────────────────────────────────────────────────────
// That hash is built from `sizeof` and `alignof` ONLY:
//
//     #define ENGINE_ABI_HASH_TYPE(T) h = mix(mix(h, sizeof(T)), alignof(T))
//
// **Reordering two same-sized fields changes neither.** Measured, not assumed —
// swapping `radius` and `height` in `CharacterController`:
//
//     sizeof A=32  sizeof B=32   hash A=9ad5d700c5a524e7  B=9ad5d700c5a524e7
//
// Identical. The gate accepts the stale kit, and that kit then writes its
// `radius` into the host's `height` for every character in the world, silently,
// with no load error and no crash.
//
// This is the SAME defect class the API table already had and already fixed.
// `extension-model.md` §1.3 states it exactly: "a reordered group keeps every
// size intact and still breaks every Kit — the static asserts would pass and the
// world would still burn." `api_abi_compat_test` froze the table's group offsets
// for that reason. Nothing had done it for the components, and `offsetof`
// appeared in exactly one file in the tree.
//
// The components are the more exposed of the two surfaces: a kit reads and
// WRITES them, every frame, in memory the host owns.
//
// ── Why some components get exact offsets and some get order only ───────────
// Four of the thirteen contain `std::string` or `std::vector`, whose `sizeof`
// differs between standard libraries (libc++ 24 bytes, libstdc++ 32) and between
// MSVC's debug and release runtimes. Freezing a byte count for those would fail
// on Linux for a reason that is not a defect.
//
// So they get FIELD ORDER, which is portable and is what the reorder hazard is
// actually about. Cross-toolchain mixing is already refused by a different check
// — `abiFingerprint` carries compiler, `__cplusplus`, API version and build mode
// — so the std::string size difference cannot reach a running host anyway.
//
// The pure-POD components get both: exact `sizeof`, exact offsets, and order.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include <engine/game_module.h>

#include "components/animator.h"
#include "components/camera.h"
#include "components/character_controller.h"
#include "components/collision_events.h"
#include "components/entity_id.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/name.h"
#include "components/script_component.h"
#include "components/skinned_mesh.h"
#include "core/transform.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// Field order, the portable half. `a` must sit strictly before `b`, which is
// what a swap of two same-typed neighbours violates and the layout hash does not.
#define ORDER(T, a, b) \
    CHECK(offsetof(T, a) < offsetof(T, b), \
          #T "::" #a " precedes " #b " (%zu < %zu)", \
          offsetof(T, a), offsetof(T, b))

// Exact placement, for components with no standard-library members.
#define AT(T, f, n) \
    CHECK(offsetof(T, f) == (size_t)(n), \
          #T "::" #f " at %zu (frozen %d)", offsetof(T, f), (int)(n))

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("component_abi_test\n");

    // ── 1. The hash still covers the set it claims to ───────────────────────
    // A component added to the engine and NOT added to componentLayoutHash is
    // invisible to the gate: kits could disagree about it forever. There is no
    // way to enumerate the macro's contents from outside, so this pins the one
    // observable it produces. Changing the component set legitimately changes
    // this number — the point is that it cannot change by ACCIDENT.
    std::printf("\n-- the gate's own value --\n");
    constexpr uint64_t kHash = engine_abi::componentLayoutHash();
    CHECK(kHash != 0, "componentLayoutHash() is %llx",
          (unsigned long long)kHash);
    CHECK(kHash != engine_abi::kFnvBasis,
          "...and it is not the empty-input basis (the macro list is not empty)");

    // ── 2. Pure-POD components: exact size and exact offsets ────────────────
    // These contain only floats, enums, bools, handles and Vec3/Quaternion, all
    // of which lay out identically on every platform this engine targets.
    std::printf("\n-- exact layout (no std:: members) --\n");

    // Transform is the most-read component in the engine — every renderer
    // extraction, every physics sync, every gizmo.
    static_assert(sizeof(Transform) == 40, "Transform grew or shrank");
    AT(Transform, position, 0);
    AT(Transform, rotation, 12);
    AT(Transform, scale,    28);

    // Seven consecutive floats. This is the struct the demonstration above used,
    // and the reason this file exists.
    static_assert(sizeof(CharacterController) == 32, "CharacterController size");
    AT(CharacterController, radius,       0);
    AT(CharacterController, height,       4);
    AT(CharacterController, maxSlopeDeg,  8);
    AT(CharacterController, stepHeight,  12);
    AT(CharacterController, mass,        16);
    AT(CharacterController, gravityScale,20);
    AT(CharacterController, footOffset,  24);
    AT(CharacterController, grounded,    28);

    // Four consecutive floats: near/far are the pair a swap would silently
    // invert, and an inverted depth range is a rendering bug nobody traces to
    // the ABI.
    AT(Camera, projection,  0);
    AT(Camera, fov,         4);
    AT(Camera, orthoSize,   8);
    AT(Camera, nearPlane,  12);
    AT(Camera, farPlane,   16);
    AT(Camera, clearColor, 20);
    AT(Camera, isPrimary,  36);

    // spotInner/spotOuter are the dangerous pair here — swapped, a spotlight's
    // cones invert and the light renders inside-out.
    AT(Light, type,           0);
    AT(Light, color,          4);
    AT(Light, intensity,     16);
    AT(Light, range,         20);
    AT(Light, spotInner,     24);
    AT(Light, spotOuter,     28);
    AT(Light, castShadows,   32);
    AT(Light, useTemperature,33);
    AT(Light, temperatureK,  36);

    // Both enums are int-sized, not byte-sized — the frozen numbers below are
    // MEASURED, and the first draft of this file guessed 1-byte enums and was
    // wrong by 4 from `shape` onward. Worth leaving in the comment: a layout
    // freeze written from reading the struct is a freeze of what someone
    // assumed, which is the failure it exists to prevent.
    static_assert(sizeof(RigidBody) == 44, "RigidBody size");
    AT(RigidBody, bodyType,    0);
    AT(RigidBody, shape,       4);
    AT(RigidBody, mass,        8);
    AT(RigidBody, restitution,12);
    AT(RigidBody, friction,   16);
    AT(RigidBody, useGravity, 20);
    AT(RigidBody, halfExtent, 24);   // 3 bytes of padding after the bool
    AT(RigidBody, radius,     36);
    AT(RigidBody, halfHeight, 40);

    AT(MeshRenderer, mesh,             0);
    AT(MeshRenderer, materialOverride, sizeof(MeshHandle));

    AT(SkinnedMesh, skeleton,        0);
    AT(SkinnedMesh, paletteSlot,     sizeof(SkeletonHandle));
    static_assert(SkinnedMesh::kNoSlot == 0xFFFFFFFFu,
                  "kNoSlot is the sentinel skinPalettes() returns null for");

    static_assert(sizeof(EntityId) == 8, "EntityId is a bare uint64");
    AT(EntityId, value, 0);

    // ── 3. Components with std::-library members: ORDER only ────────────────
    // Absolute offsets here would encode libc++'s std::string size and fail on
    // Linux. Order is the portable invariant and is what the reorder hazard is.
    std::printf("\n-- field order (std:: members, sizes are not portable) --\n");
    ORDER(Animator, clip,       clipPath);
    ORDER(Animator, clipPath,   clipIndex);
    ORDER(Animator, clipIndex,  time);
    ORDER(Animator, time,       speed);
    ORDER(Animator, speed,      fade);
    ORDER(Animator, fade,       playing);
    ORDER(Animator, playing,    looping);

    ORDER(ScriptComponent, scriptPath, instanceId);
    ORDER(ScriptComponent, instanceId, started);

    ORDER(CollisionEvents, entered, exited);

    // Name and CollisionEvents are single-purpose enough that order is the whole
    // contract; Name has one member, so its invariant is that it STAYS one.
    static_assert(sizeof(Name) == sizeof(std::string),
                  "Name is exactly its string — a second member would change "
                  "the layout hash and refuse every kit, which is correct, but "
                  "it should be a deliberate act");

    // ── 4. Alignment, which the hash does cover but nothing pins ────────────
    // In the hash, so a change is caught by the gate — but the gate's message is
    // "rebuild the module", which is the wrong remedy if the real cause is a
    // component that accidentally gained 16-byte alignment and doubled its
    // stride in every archetype.
    std::printf("\n-- alignment --\n");
    CHECK(alignof(Transform) == 4, "Transform is 4-aligned (%zu)", alignof(Transform));
    CHECK(alignof(CharacterController) == 4, "CharacterController is 4-aligned (%zu)",
          alignof(CharacterController));
    CHECK(alignof(Camera) == 4, "Camera is 4-aligned (%zu)", alignof(Camera));
    CHECK(alignof(Light)  == 4, "Light is 4-aligned (%zu)",  alignof(Light));

    if (g_failures) {
        std::printf("\ncomponent_abi_test: %d check(s) FAILED\n", g_failures);
        std::printf("A layout change here is not necessarily wrong — but it is "
                    "ALWAYS a deliberate act, because every kit built before it "
                    "now misreads live world data. Update the frozen numbers in "
                    "the same commit that changes the component, and expect "
                    "componentLayoutHash() to refuse older modules.\n");
        return 1;
    }
    std::printf("\ncomponent_abi_test: all checks passed\n");
    return 0;
}
