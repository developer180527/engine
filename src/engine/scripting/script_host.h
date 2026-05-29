#pragma once
#include <flecs.h>
#include <cctype>
#include <cstdint>
#include "core/transform.h"
#include "components/name.h"
#include "engine/input.h"
#include "engine/input_event.h"
#include "engine/logger.h"
#include "engine/scripting/script_services.h"

// ── keyFromName ────────────────────────────────────────────────────────────
// Map a script-facing key name ("W", "Space", "Left") to a Key. Key mirrors
// GLFW codes, so single letters/digits ARE their ASCII uppercase code and
// named keys use documented GLFW3 constants — no dependency on enum member
// names. Unknown -> (Key)0, a never-set slot, so isKeyDown returns false.
inline Key keyFromName(const char* name) {
    if (!name || !name[0]) return (Key)0;
    if (!name[1]) { // single char
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (Key)c;
    }
    struct Named { const char* n; int code; };
    static const Named kTable[] = {
        {"Space",32},{"Escape",256},{"Enter",257},{"Return",257},{"Tab",258},
        {"Backspace",259},{"Delete",261},{"Right",262},{"Left",263},
        {"Down",264},{"Up",265},
        {"LeftShift",340},{"LeftControl",341},{"LeftCtrl",341},{"LeftAlt",342},
        {"RightShift",344},{"RightControl",345},{"RightAlt",346},
    };
    for (const auto& k : kTable) {
        const char* a = name; const char* b = k.n; bool eq = true;
        while (*a && *b) { if (std::tolower(*a) != std::tolower(*b)) { eq = false; break; } ++a; ++b; }
        if (eq && !*a && !*b) return (Key)k.code;
    }
    return (Key)0;
}

// ── ScriptHost ─────────────────────────────────────────────────────────────
// THE scripting contract. The engine owns one ScriptHost per running world.
// Every backend (Lua, Python, blueprint) is a frontend that exposes these
// methods to its language. Keep this surface STABLE and MINIMAL — changing it
// breaks scripts in every language at once.
//
// Threading/iteration note for backends: create/destroy/setParent are
// structural ECS changes. Backends iterating script entities must perform them
// outside a locked query (defer_begin/defer_end) — same rule as the editor.
class ScriptHost {
public:
    explicit ScriptHost(flecs::world* world) : m_world(world) {}
    void setWorld(flecs::world* w) { m_world = w; } // bound per Play
    void     beginSession() { ++m_epoch; } // invalidates entity refs from prior plays
    uint32_t epoch() const  { return m_epoch; }

    // Backend sets per-frame state before dispatching onUpdate.
    void setFrame(float dt, double elapsed, uint64_t frame) {
        m_dt = dt; m_elapsed = elapsed; m_frame = frame;
    }

    // ── Entity ──────────────────────────────────────────────────────────
    flecs::entity create(const char* name) {
        Transform t{}; t.position = {0,0,0}; t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        return m_world->entity(name).set<Transform>(t).set<Name>({name});
    }
    void          destroy(flecs::entity e)            { if (e.is_alive()) e.destruct(); }
    flecs::entity find(const char* name)              { return m_world->lookup(name); }
    bool          isAlive(flecs::entity e) const      { return e.is_alive(); }
    void          setParent(flecs::entity c, flecs::entity p) {
        if (!c.is_alive() || !p.is_alive() || c == p) return;
        // Refuse cycles: if p is a descendant of c, parenting would make c
        // its own ancestor and getWorldMatrix would infinite-recurse.
        int guard = 0;
        for (flecs::entity a = p; a && a.is_alive() && guard < 4096;
             a = a.target(flecs::ChildOf), ++guard)
            if (a == c) return;
        c.remove(flecs::ChildOf, flecs::Wildcard);
        c.add(flecs::ChildOf, p);
    }
    void          clearParent(flecs::entity c)        { if (c.is_alive()) c.remove(flecs::ChildOf, flecs::Wildcard); }

    // ── Transform (only typed component for v1) ─────────────────────────
    bool hasTransform(flecs::entity e) const          { return e.is_alive() && e.has<Transform>(); }
    bool getTransform(flecs::entity e, Transform& out) const {
        if (!e.is_alive()) return false;
        const Transform* t = e.try_get<Transform>();
        if (!t) return false;
        out = *t; return true;
    }
    void setTransform(flecs::entity e, const Transform& t) { if (e.is_alive()) e.set<Transform>(t); }

    // ── Input ───────────────────────────────────────────────────────────
    bool  keyDown(const char* key)    const { return Input::isKeyDown(keyFromName(key)); }
    bool  keyPressed(const char* key) const { return Input::isKeyPressed(keyFromName(key)); }
    float axis(const char* name)      const { return Input::getAxis(name); }
    bool  mouseDown(int button)       const { return Input::isMouseDown((MouseButton)button); }
    void  mouseDelta(float& dx, float& dy) const { dx = Input::mouseDeltaX(); dy = Input::mouseDeltaY(); }

    // ── Time ────────────────────────────────────────────────────────────
    float    dt()      const { return m_dt; }
    double   elapsed() const { return m_elapsed; }
    uint64_t frame()   const { return m_frame; }

    // ── Log ─────────────────────────────────────────────────────────────
    void logInfo (const char* m) const { LOG_INFO ("Script", "%s", m); }
    void logWarn (const char* m) const { LOG_WARN ("Script", "%s", m); }
    void logError(const char* m) const { LOG_ERROR("Script", "%s", m); }

    // ── Physics (RESERVED — no-op until a service is registered) ────────
    void applyImpulse(flecs::entity e, float x, float y, float z) { if (m_physics) m_physics->applyImpulse(e,x,y,z); }
    void setVelocity (flecs::entity e, float x, float y, float z) { if (m_physics) m_physics->setVelocity(e,x,y,z); }
    bool getVelocity (flecs::entity e, float& x, float& y, float& z) { return m_physics ? m_physics->getVelocity(e,x,y,z) : false; }
    RaycastHit raycast(float ox,float oy,float oz, float dx,float dy,float dz, float maxDist) {
        return m_physics ? m_physics->raycast(ox,oy,oz,dx,dy,dz,maxDist) : RaycastHit{};
    }
    void setPhysicsService(IPhysicsService* s) { m_physics = s; }

    // ── Audio (RESERVED — no-op until a service is registered) ──────────
    uint32_t playSound  (const char* path)                          { return m_audio ? m_audio->play(path) : 0; }
    uint32_t playSoundAt(const char* path, float x, float y, float z) { return m_audio ? m_audio->playAt(path,x,y,z) : 0; }
    void     stopSound  (uint32_t handle)                           { if (m_audio) m_audio->stop(handle); }
    void     setAudioService(IAudioService* s) { m_audio = s; }

    flecs::world* world() const { return m_world; }

private:
    flecs::world*    m_world   = nullptr;
    IPhysicsService* m_physics = nullptr; // null until Jolt service lands
    IAudioService*   m_audio   = nullptr; // null until miniaudio lands
    float    m_dt      = 0.0f;
    double   m_elapsed = 0.0;
    uint64_t m_frame   = 0;
    uint32_t m_epoch   = 0;
};
