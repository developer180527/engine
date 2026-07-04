#pragma once
#include <flecs.h>
#include <cstdint>

// ── Reserved engine-service interfaces ─────────────────────────────────────
// The ScriptHost routes physics/audio calls to these. They are NULL until a
// concrete backend registers one (Jolt for physics, miniaudio for audio).
// Declared now so the scripting contract is stable: scripts may call
// Physics.applyImpulse / Audio.play today; the calls safely no-op until the
// backing service exists, and "just work" once it does — no contract change.

// Result of a world raycast. `entity` is the flecs entity id of the body hit
// (0 if nothing / unmapped). point + normal are world space; distance is along
// the ray from the origin.
struct RaycastHit {
    bool     hit       = false;
    uint64_t entity    = 0;
    float    point[3]  = {0,0,0};
    float    normal[3] = {0,0,0};
    float    distance  = 0.0f;
};

// Services take an EXPLICIT (world&, entity_t) rather than a flecs::entity.
// A flecs::entity silently bundles a world pointer; passing the bare 64-bit id
// plus the caller's world keeps a backend from depending on which world an
// entity happened to be wrapped against, and matches the C API, which already
// trades in EngineEntity ids. A backend calls world.entity(e) only where it
// needs component access. (Audio takes no entity — its interface is unchanged.)
struct IAnimService {
    virtual ~IAnimService() = default;
    virtual bool  play(flecs::world& w, flecs::entity_t e, const char* clipPath, float fade) = 0;
    virtual void  setSpeed(flecs::world& w, flecs::entity_t e, float s)        = 0;
    virtual void  setLooping(flecs::world& w, flecs::entity_t e, bool loop)    = 0;
    virtual void  setPlaying(flecs::world& w, flecs::entity_t e, bool playing) = 0;
    virtual bool  isPlaying(flecs::world& w, flecs::entity_t e) const          = 0;
    virtual float time(flecs::world& w, flecs::entity_t e) const               = 0;
    virtual float duration(flecs::world& w, flecs::entity_t e) const           = 0;
};

struct IPhysicsService {
    virtual ~IPhysicsService() = default;
    virtual void applyImpulse(flecs::world& w, flecs::entity_t e, float x, float y, float z)    = 0;
    virtual void setVelocity (flecs::world& w, flecs::entity_t e, float x, float y, float z)    = 0;
    virtual bool getVelocity (flecs::world& w, flecs::entity_t e, float& x, float& y, float& z) = 0;
    virtual RaycastHit raycast(float ox, float oy, float oz,
                               float dx, float dy, float dz, float maxDist)      = 0;

    // Character controller (default no-op so non-Jolt backends compile as-is)
    virtual void charMove (flecs::world& /*w*/, flecs::entity_t /*e*/, float /*vx*/, float /*vz*/) {}
    virtual void charJump (flecs::world& /*w*/, flecs::entity_t /*e*/, float /*speed*/)            {}
    virtual bool charIsGrounded(flecs::world& /*w*/, flecs::entity_t /*e*/)                        { return false; }
};

struct IAudioService {
    virtual ~IAudioService() = default;
    virtual uint32_t play  (const char* path)                                    = 0;
    virtual uint32_t playAt(const char* path, float x, float y, float z)         = 0;
    virtual void     stop  (uint32_t handle)                                     = 0;
};

// Game-world singleton: lets the script backend locate the active physics
// service without a compile-time dependency on the physics plugin. The physics
// backend publishes itself here on simulation start.
struct PhysicsServiceRef { IPhysicsService* svc = nullptr; };
struct AudioServiceRef   { IAudioService*   svc = nullptr; };
