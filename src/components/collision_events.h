#pragma once
#include <vector>
#include <flecs.h>

// ── CollisionEvents ────────────────────────────────────────────────────────
// Set by JoltPlugin::flushCollisionEvents() on entities that had contact
// events this frame. Removed when no events remain (clean archetypes).
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
