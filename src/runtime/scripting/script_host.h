#pragma once
#include <unordered_map>
#include <memory>
#include <flecs.h>
#include <cctype>
#include <cstdint>
#include "core/transform.h"
#include "core/transform_utils.h"   // safeReparent (depth/cycle-guarded parenting)
#include "core/debug_draw.h"
#include "components/name.h"
#include "runtime/input/input.h"
#include "runtime/input/input_event.h"
#include "core/logger.h"
#include "runtime/scripting/script_services.h"
#include "runtime/platform/platform.h"
#include "runtime/world_query_cache.h"
#include "runtime/services/asset_service.h"
#include "project/project_context.h"
#include "runtime/services/scene_service.h"
#include "runtime/services/nav_service.h"

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
        // Destroy the PREVIOUS bind's observers first. "Observers die with
        // the world" is only true if the world dies before the next bind —
        // false in InPlace mode, where every Play/Stop re-binds the immortal
        // m_ecs (2 leaked observers per cycle), and lethal at shutdown: the
        // OnRemove lambda captures `this`, and ~EngineRuntime destroys the
        // ScriptHost BEFORE m_ecs (reverse member order), so m_ecs tearing
        // down Name entities would fire into a freed host. Call sites keep
        // the outgoing world alive across this call (stopSimulation unbinds
        // before m_gameWorld.reset()), making destruct() safe here.
        if (m_nameObsSet)    { m_nameObsSet.destruct();    m_nameObsSet    = flecs::entity(); }
        if (m_nameObsRemove) { m_nameObsRemove.destruct(); m_nameObsRemove = flecs::entity(); }
        // Belt-and-braces: if a host is ever destroyed while still bound
        // (destruct() above never ran), surviving lambdas must go inert
        // instead of touching the dead host. They hold a weak token; the
        // token dies with the host (and is renewed per bind).
        m_aliveToken = std::make_shared<char>();
        m_world = w;
        m_nameQuery.reset();   // old world may be destroyed — drop its query
        // Name index: O(1) find() (the linear Name scan was a sleeper once
        // spawners exist). Observers keep it fresh. Primed with pre-existing
        // names.
        m_nameIndex.clear();
        if (!w) return;
        std::weak_ptr<char> alive = m_aliveToken;
        m_nameObsSet = w->observer<const Name>().event(flecs::OnSet)
            .each([this, alive](flecs::entity e, const Name& n) {
                if (alive.expired()) return;   // host died while bound
                m_nameIndex[n.value] = e.id();
            });
        m_nameObsRemove = w->observer<const Name>().event(flecs::OnRemove)
            .each([this, alive](flecs::entity e, const Name& n) {
                if (alive.expired()) return;   // world outlived the host
                auto it = m_nameIndex.find(n.value);
                if (it != m_nameIndex.end() && it->second == e.id())
                    m_nameIndex.erase(it);
            });
        w->each([this](flecs::entity e, const Name& n) {
            m_nameIndex[n.value] = e.id();
        });
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
        // name. HOT PATH: the observer-maintained index answers in O(1);
        // hits are re-verified (renames leave stale keys) and healed.
        auto it = m_nameIndex.find(name);
        if (it != m_nameIndex.end()) {
            flecs::entity e = m_world->entity(it->second);
            const Name* n = e.is_alive() ? e.try_get<Name>() : nullptr;
            if (n && n->value == name) return e;
            m_nameIndex.erase(it);   // stale (renamed/died) — heal
        }
        // COLD: full scan, healing the index on a hit.
        flecs::entity found;
        m_nameQuery.get(*m_world).each([&](flecs::entity e, const Name& n) {
            if (!found && n.value == name) found = e;
        });
        if (found) { m_nameIndex[name] = found.id(); return found; }
        return m_world->lookup(name); // flecs-named (script-created) entities
    }
    bool          isAlive(flecs::entity e) const      { return e.is_alive(); }
    void          setParent(flecs::entity c, flecs::entity p) {
        safeReparent(c, p);   // refuses cycles + over-deep chains (flecs abort)
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
    // Split the incoming entity into the (world, id) pair the services take —
    // sourced from the host's bound world, not the entity's bundled one.
    void applyImpulse(flecs::entity e, float x, float y, float z) { if (auto* p = physicsOrWarn()) p->applyImpulse(*m_world,e.id(),x,y,z); }
    void setVelocity (flecs::entity e, float x, float y, float z) { if (auto* p = physicsOrWarn()) p->setVelocity(*m_world,e.id(),x,y,z); }
    bool getVelocity (flecs::entity e, float& x, float& y, float& z) { auto* p = physicsOrWarn(); return p ? p->getVelocity(*m_world,e.id(),x,y,z) : false; }
    RaycastHit raycast(float ox,float oy,float oz, float dx,float dy,float dz, float maxDist) {
        auto* p = physicsOrWarn();
        return p ? p->raycast(ox,oy,oz,dx,dy,dz,maxDist) : RaycastHit{};
    }
    void setPhysicsService(IPhysicsService* s) { m_physics = s; if (s) m_warnedPhysics = false; }

    // Character controller (same warn-once contract)
    void charMove(flecs::entity e, float vx, float vz) { if (auto* p = physicsOrWarn()) p->charMove(*m_world,e.id(),vx,vz); }
    void charJump(flecs::entity e, float speed)        { if (auto* p = physicsOrWarn()) p->charJump(*m_world,e.id(),speed); }
    bool charGrounded(flecs::entity e)                 { auto* p = physicsOrWarn(); return p ? p->charIsGrounded(*m_world,e.id()) : false; }

    // ── Audio (no-op + ONE warning until a service is registered) ───────
    uint32_t playSound  (const char* path)                          { auto* a = audioOrWarn(); return a ? a->play(path) : 0; }
    uint32_t playSoundAt(const char* path, float x, float y, float z) { auto* a = audioOrWarn(); return a ? a->playAt(path,x,y,z) : 0; }
    void     stopSound  (uint32_t handle)                           { if (auto* a = audioOrWarn()) a->stop(handle); }
    void     setAudioService(IAudioService* s) { m_audio = s; if (s) m_warnedAudio = false; }

    // ── Navigation (engine-owned NavService; no-op until a navmesh is baked) ──
    void setNavService(nav::NavService* n) { m_nav = n; }
    // Named locals rather than `(const float[3]){...}` compound literals:
    // those are C99, accepted by Clang in C++ only as a GNU extension, and
    // rejected outright by GCC ("taking address of temporary array").
    int  navFindPath(float sx, float sy, float sz, float ex, float ey, float ez,
                     float* out, int maxPoints) const {
        if (!m_nav) return 0;
        const float start[3] = { sx, sy, sz };
        const float end[3]   = { ex, ey, ez };
        return m_nav->findPath(start, end, out, maxPoints);
    }
    bool navProject(float x, float y, float z, float* out) const {
        if (!m_nav) return false;
        const float p[3] = { x, y, z };
        return m_nav->projectPoint(p, out);
    }
    bool navReady() const { return m_nav && m_nav->ready(); }

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

    // ── Debug draw (engineDraw* -> per-frame line collector) ───────────────
    void setDebugDraw(dbg::DebugDraw* d) { m_debugDraw = d; }
    dbg::DebugDraw* debugDraw() const { return m_debugDraw; }

    // ── Animation — DELEGATED to the runtime's IAnimService ────────────────
    // ScriptHost is a coordinator, not an implementation (god-object guard,
    // per the July 2026 API review): the C API contract stays here, the
    // policy lives in runtime/services/anim_service.h.
    void setAnimService(IAnimService* s) { m_anim = s; }

    bool animPlay(flecs::entity e, const char* clipPath, float fade) {
        return m_anim && m_anim->play(*m_world, e.id(), clipPath, fade);
    }
    void animSetSpeed(flecs::entity e, float s)    { if (m_anim) m_anim->setSpeed(*m_world, e.id(), s); }
    void animSetLooping(flecs::entity e, bool l)   { if (m_anim) m_anim->setLooping(*m_world, e.id(), l); }
    void animSetPlaying(flecs::entity e, bool p)   { if (m_anim) m_anim->setPlaying(*m_world, e.id(), p); }
    bool animIsPlaying(flecs::entity e) const      { return m_anim && m_anim->isPlaying(*m_world, e.id()); }
    float animTime(flecs::entity e) const          { return m_anim ? m_anim->time(*m_world, e.id()) : 0.0f; }
    float animDuration(flecs::entity e) const      { return m_anim ? m_anim->duration(*m_world, e.id()) : 0.0f; }

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
    dbg::DebugDraw*  m_debugDraw    = nullptr; // per-frame line collector (engineDraw*)
    IAnimService*         m_anim       = nullptr; // runtime-owned AnimService
    IPhysicsService* m_physics      = nullptr; // null until Jolt service lands
    IAudioService*   m_audio        = nullptr; // null until miniaudio lands
    nav::NavService* m_nav          = nullptr; // engine-owned; set at init
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
    std::unordered_map<std::string, flecs::entity_t> m_nameIndex; // O(1) find
    flecs::entity m_nameObsSet{};     // observer entities, owned: destructed
    flecs::entity m_nameObsRemove{};  // on re-bind (see setWorld comment)
    std::shared_ptr<char> m_aliveToken = std::make_shared<char>();
    float    m_dt      = 0.0f;
    double   m_elapsed = 0.0;
    uint64_t m_frame   = 0;
    uint32_t m_epoch   = 0;
};
