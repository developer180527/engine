#pragma once
#include <bx/math.h>

enum class PhysicsBodyType { Static, Kinematic, Dynamic };
enum class PhysicsShape    { Box, Sphere, Capsule };

// ── RigidBody ──────────────────────────────────────────────────────────────
// Add to any entity to make it participate in physics simulation.
// JoltPlugin reads this on onSimulationStart() and creates a Jolt body.
// During simulation, Transform is written back from Jolt every frame
// for Dynamic and Kinematic bodies.
struct RigidBody {
    PhysicsBodyType bodyType    = PhysicsBodyType::Dynamic;
    PhysicsShape    shape       = PhysicsShape::Box;
    float           mass        = 1.0f;
    float           restitution = 0.3f;
    float           friction    = 0.6f;
    bool            useGravity  = true;
    // Box: half-extents on each axis
    // Sphere: only radius used
    // Capsule: radius + halfHeight used
    bx::Vec3        halfExtent  = {0.5f, 0.5f, 0.5f};
    float           radius      = 0.5f;
    float           halfHeight  = 0.5f;
};
