#pragma once
#include <flecs.h>
#include <cctype>
#include <cstdint>
#include "core/transform.h"
#include "components/name.h"
#include "runtime/input/input.h"
#include "runtime/input/input_event.h"
#include "core/logger.h"
#include "runtime/scripting/script_services.h"
#include "runtime/platform/platform.h"
#include "runtime/world_query_cache.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"

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
    void setWorld(flecs::world* w) {                // bound per Play
        m_world = w;
        m_nameQuery.reset();   // old world may be destroyed — drop its query
    }
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
    flecs::entity find(const char* name) {
        // Scene entities are named via the Name component, not flecs' builtin
        // name (load creates them anonymous), so search Name first.
        // Scripts call this per frame — the query is cached per world.
        flecs::entity found;
        m_nameQuery.get(*m_world).each([&](flecs::entity e, const Name& n) {
            if (!found && n.value == name) found = e;
        });
        if (found) return found;
        return m_world->lookup(name); // fallback: flecs-named (script-created) entities
    }
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

    // ── Physics (no-op + ONE warning until a service is registered) ─────
    // The optional services fail safe but not silent: the first call against
    // an unbound service logs once, so "my impulse does nothing" is a log
    // line away from "is JoltPlugin attached / is the simulation running?".
    void applyImpulse(flecs::entity e, float x, float y, float z) { if (auto* p = physicsOrWarn()) p->applyImpulse(e,x,y,z); }
    void setVelocity (flecs::entity e, float x, float y, float z) { if (auto* p = physicsOrWarn()) p->setVelocity(e,x,y,z); }
    bool getVelocity (flecs::entity e, float& x, float& y, float& z) { auto* p = physicsOrWarn(); return p ? p->getVelocity(e,x,y,z) : false; }
    RaycastHit raycast(float ox,float oy,float oz, float dx,float dy,float dz, float maxDist) {
        auto* p = physicsOrWarn();
        return p ? p->raycast(ox,oy,oz,dx,dy,dz,maxDist) : RaycastHit{};
    }
    void setPhysicsService(IPhysicsService* s) { m_physics = s; if (s) m_warnedPhysics = false; }

    // Character controller (same warn-once contract)
    void charMove(flecs::entity e, float vx, float vz) { if (auto* p = physicsOrWarn()) p->charMove(e,vx,vz); }
    void charJump(flecs::entity e, float speed)        { if (auto* p = physicsOrWarn()) p->charJump(e,speed); }
    bool charGrounded(flecs::entity e)                 { auto* p = physicsOrWarn(); return p ? p->charIsGrounded(e) : false; }

    // ── Audio (no-op + ONE warning until a service is registered) ───────
    uint32_t playSound  (const char* path)                          { auto* a = audioOrWarn(); return a ? a->play(path) : 0; }
    uint32_t playSoundAt(const char* path, float x, float y, float z) { auto* a = audioOrWarn(); return a ? a->playAt(path,x,y,z) : 0; }
    void     stopSound  (uint32_t handle)                           { if (auto* a = audioOrWarn()) a->stop(handle); }
    void     setAudioService(IAudioService* s) { m_audio = s; if (s) m_warnedAudio = false; }

    // ── Assets (sync cooked-asset loading via AssetService) ────────────
    // Returns uint32_t handle IDs (0 = invalid) for FFI compatibility.
    // All load calls are synchronous — they do file I/O + bgfx handle
    // creation on the calling thread. Suitable for preload phases and
    // small assets; async loading comes in a later step.
    void setAssetService(AssetService* s) { m_assetService = s; }

    // ── Cursor (mouse-look capture) — routed to the platform ───────────────
    void setPlatform(IPlatform* p) { m_platform = p; }
    void setCursorCaptured(bool captured) {
        if (m_platform)
            m_platform->setCursorMode(captured ? CursorMode::Captured
                                               : CursorMode::Normal);
        m_cursorCaptured = captured;
    }
    bool cursorCaptured() const { return m_cursorCaptured; }

    uint32_t assetLoadMesh(const char* cookedPath) {
        return m_assetService ? m_assetService->loadMesh(cookedPath).id : 0;
    }
    bool assetUnloadMesh(uint32_t handleId) {
        return m_assetService ? m_assetService->unloadMesh(MeshHandle{handleId}) : false;
    }
    uint32_t assetLoadTexture(const char* cookedPath) {
        return m_assetService ? m_assetService->loadTexture(cookedPath).id : 0;
    }
    bool assetUnloadTexture(uint32_t handleId) {
        return m_assetService ? m_assetService->unloadTexture(TextureHandle{handleId}) : false;
    }
    bool assetUnloadMaterial(uint32_t handleId) {
        return m_assetService ? m_assetService->unloadMaterial(MaterialHandle{handleId}) : false;
    }
    // Async variants — queue load for background worker, poll with query*()
    void assetLoadMeshAsync(const char* cookedPath) {
        if (m_assetService) m_assetService->loadMeshAsync(cookedPath);
    }
    void assetLoadTextureAsync(const char* cookedPath) {
        if (m_assetService) m_assetService->loadTextureAsync(cookedPath);
    }
    uint32_t assetQueryMesh(const char* cookedPath) const {
        return m_assetService ? m_assetService->queryMesh(cookedPath) : 0;
    }
    uint32_t assetQueryTexture(const char* cookedPath) const {
        return m_assetService ? m_assetService->queryTexture(cookedPath) : 0;
    }
    bool assetIsLoading(const char* cookedPath) const {
        return m_assetService ? m_assetService->isLoading(cookedPath) : false;
    }

    size_t assetMeshCount()     const { return m_assetService ? m_assetService->meshCount()     : 0; }
    size_t assetTextureCount()  const { return m_assetService ? m_assetService->textureCount()  : 0; }
    size_t assetMaterialCount() const { return m_assetService ? m_assetService->materialCount() : 0; }

    // ── Scenes (binary scene loading via SceneService) ────────────────
    void setSceneService(SceneService* s) { m_sceneService = s; }

    uint32_t sceneLoad(const char* cookedPath) {
        return m_sceneService ? m_sceneService->loadScene(cookedPath) : 0;
    }
    bool sceneUnload(uint32_t handle) {
        return m_sceneService ? m_sceneService->unloadScene(handle) : false;
    }
    void scenePreload(const char* cookedPath) {
        if (m_sceneService) m_sceneService->preloadScene(cookedPath);
    }
    bool sceneIsReady(const char* cookedPath) const {
        return m_sceneService ? m_sceneService->isSceneReady(cookedPath) : false;
    }
    uint32_t sceneEntityCount(uint32_t handle) const {
        return m_sceneService ? m_sceneService->sceneEntityCount(handle) : 0;
    }
    uint32_t sceneActiveCount() const {
        return m_sceneService ? m_sceneService->activeSceneCount() : 0;
    }

    flecs::world* world() const { return m_world; }

private:
    flecs::world*    m_world        = nullptr;
    IPlatform*       m_platform     = nullptr; // for cursor capture
    bool             m_cursorCaptured = false;
    IPhysicsService* m_physics      = nullptr; // null until Jolt service lands
    IAudioService*   m_audio        = nullptr; // null until miniaudio lands
    AssetService*    m_assetService = nullptr; // null until wired from EngineContext
    SceneService*    m_sceneService = nullptr; // null until wired from EngineContext
    IPhysicsService* physicsOrWarn() {
        if (!m_physics && !m_warnedPhysics) {
            m_warnedPhysics = true;
            LOG_WARN("Script", "physics API called but no physics service is "
                     "bound — is a physics plugin attached and the simulation "
                     "running? (warning shown once)");
        }
        return m_physics;
    }
    IAudioService* audioOrWarn() {
        if (!m_audio && !m_warnedAudio) {
            m_warnedAudio = true;
            LOG_WARN("Script", "audio API called but no audio service is "
                     "bound — is an audio plugin attached and the simulation "
                     "running? (warning shown once)");
        }
        return m_audio;
    }

    bool m_warnedPhysics = false;
    bool m_warnedAudio   = false;
    WorldQueryCache<const Name> m_nameQuery;
    float    m_dt      = 0.0f;
    double   m_elapsed = 0.0;
    uint64_t m_frame   = 0;
    uint32_t m_epoch   = 0;
};
