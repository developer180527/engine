#pragma once
#include <cstdint>
#include <vector>
#include <flecs.h>

// ── CollisionEvents ────────────────────────────────────────────────────────
// Set by JoltPlugin::flushCollisionEvents() on a body at its first contact,
// and KEPT from then on: on a tick with no contact change both lists are
// empty. "Has CollisionEvents" means "has touched something, ever", not "had
// an event this tick"; test hasEnter()/hasExit() (WO-048, revision 1).
//
// GAME CODE CONTRACT:
//   Always validate handles before use — an entity may be deleted between
//   physics step and your system running:
//
//   for (auto other_id : ce.entered)
//       if (ecs.entity(other_id).is_alive())
//           doSomething(ecs.entity(other_id));
//
// SYSTEM ORDER:
//   Scripts/gameplay systems must run AFTER broadcastUpdate() (which calls
//   flushCollisionEvents) and BEFORE the next frame's broadcastUpdate to
//   read a complete, consistent event set. With flecs pipelines, schedule
//   your script system after the PhysicsUpdate phase.
struct CollisionEvents {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    // Revision 1 (WO-048): the component stays on with empty lists, where
    // revision 0 removed it on a quiet tick.
    static constexpr uint32_t kAbiRevision = 1;

    std::vector<flecs::entity_t> entered; // bodies that started contact this frame
    std::vector<flecs::entity_t> exited;  // bodies that lost contact this frame

    // THIS TICK's contacts that started (entered) and ended (exited), in a
    // deterministic order (sorted at the source, BUG-0054). A body gets the
    // component at its first contact and KEEPS it: JoltPlugin clears the lists
    // in place every tick and refills them, so "no event this tick" is two
    // empty lists, never a missing component, and no tick makes a structural
    // change (WO-048). Capacity persists across ticks; the reserve below
    // covers typical contact counts from the first one.
    CollisionEvents() { entered.reserve(8); exited.reserve(8); }

    bool hasEnter() const { return !entered.empty(); }
    bool hasExit()  const { return !exited.empty();  }
};
