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

    // Pre-allocate for typical contact counts — avoids heap alloc on first collision.
    // For entities that already have this component, JoltPlugin updates vectors
    // in-place (clear + insert) so capacity persists. For newly colliding entities,
    // a fresh CollisionEvents is set; for entities with no events this frame,
    // the component is removed (deferred, so archetypes stay lean).
    CollisionEvents() { entered.reserve(8); exited.reserve(8); }

    bool hasEnter() const { return !entered.empty(); }
    bool hasExit()  const { return !exited.empty();  }
};
