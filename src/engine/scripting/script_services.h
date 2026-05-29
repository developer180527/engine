#pragma once
#include <flecs.h>
#include <cstdint>

// ── Reserved engine-service interfaces ─────────────────────────────────────
// The ScriptHost routes physics/audio calls to these. They are NULL until a
// concrete backend registers one (Jolt for physics, miniaudio for audio).
// Declared now so the scripting contract is stable: scripts may call
// Physics.applyImpulse / Audio.play today; the calls safely no-op until the
// backing service exists, and "just work" once it does — no contract change.

struct IPhysicsService {
    virtual ~IPhysicsService() = default;
    virtual void applyImpulse(flecs::entity e, float x, float y, float z)        = 0;
    virtual void setVelocity (flecs::entity e, float x, float y, float z)        = 0;
    virtual bool getVelocity (flecs::entity e, float& x, float& y, float& z)     = 0;
    // Reserved: raycast(origin, dir, maxDist) -> hit — added with Jolt plumbing.
};

struct IAudioService {
    virtual ~IAudioService() = default;
    virtual uint32_t play  (const char* path)                                    = 0;
    virtual uint32_t playAt(const char* path, float x, float y, float z)         = 0;
    virtual void     stop  (uint32_t handle)                                     = 0;
};
