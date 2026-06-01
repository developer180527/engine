#pragma once
// ── CharacterController ──────────────────────────────────────────────────
// Kinematic player movement via Jolt CharacterVirtual (collide-and-slide,
// slope/step handling, no rigid-body jitter). An entity has EITHER a
// RigidBody OR a CharacterController — not both. JoltPlugin creates a
// CharacterVirtual on onSimulationStart, steps it each fixed tick, and writes
// Transform.position back every frame (rotation is left to the script, so the
// player can face its movement direction).
//
// The capsule is offset up by height/2 so the reference position sits at the
// FEET — matching typical model origins. Set the entity's position to where
// the feet should rest.
struct CharacterController {
    float radius       = 0.3f;   // capsule radius
    float height       = 1.8f;   // total capsule height (cylinder + 2 hemispheres)
    float maxSlopeDeg  = 45.0f;  // steepest walkable slope
    float stepHeight   = 0.3f;   // max stair step it can climb
    float mass         = 70.0f;  // used when pushing dynamic bodies
    float gravityScale = 1.0f;   // multiplier on world gravity
    bool  grounded     = false;  // runtime-only mirror (inspector readout)
};
