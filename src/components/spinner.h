#pragma once

// Spinner component.
//
// Marks an entity as "should rotate continuously over time." The spinner
// system reads this component each frame and updates the entity's Transform
// rotation accordingly.
//
// This is the engine's first real component-driven behavior — a system that
// reads one component and writes another. Same shape as future systems will
// have: physics reads RigidBody and writes Transform, scripts read Script
// and write whatever, etc.
//
// speedYaw / speedPitch are radians per second around the Y and X axes.
struct Spinner {
    float speedYaw   = 0.0f;
    float speedPitch = 0.0f;
};
