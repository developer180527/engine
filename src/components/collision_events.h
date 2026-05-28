#pragma once
#include <vector>
#include <flecs.h>

// ── CollisionEvents ────────────────────────────────────────────────────────
// Set by JoltPlugin on entities that had contact events this frame.
// Cleared automatically at the start of the next physics update.
// Scripts and systems query this component to respond to collisions.
//
// Usage:
//   ecs.query<const CollisionEvents>().each([](flecs::entity e, const CollisionEvents& ce) {
//       for (auto other : ce.entered) { /* started colliding with 'other' */ }
//       for (auto other : ce.exited)  { /* stopped colliding with 'other' */ }
//   });
struct CollisionEvents {
    std::vector<flecs::entity_t> entered; // new contacts this frame
    std::vector<flecs::entity_t> exited;  // lost contacts this frame
    bool hasEnter() const { return !entered.empty(); }
    bool hasExit()  const { return !exited.empty();  }
};
