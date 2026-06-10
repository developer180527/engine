#pragma once
// ── EntitySerializer ─────────────────────────────────────────────────────────
// The single component (de)serialization path. Scene save/load, the in-memory
// editor->game snapshot, and the undo stack all drive this same descriptor
// table, so a component serialized in one place can never drift from another.
// Add a component once here and every path picks it up.
//
// Identity (EntityId) is owned by createEntity() via IdPolicy, not a table
// entry. Parent links (ChildOf) are restored by callers in a post-pass, since
// they require all entities to exist first.
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <filesystem>
#include <cctype>

#include "core/transform.h"
#include "core/handle.h"
#include "core/entity_id_util.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/skinned_mesh.h"
#include "components/animator.h"
#include "render/asset_registry.h"
#include "io/asset_storage.h"
#include "io/importer_registry.h"
#include "render/primitive_library.h"
#include "runtime/asset_service.h"
#include "runtime/logger.h"

namespace EntitySerde {

// Disk: meshes referenced by asset sourcePath, re-resolved on load (possibly
// async). Memory: live handle ids reused directly (instant, same-session).
enum class SerdeMode { Disk, Memory };
enum class IdPolicy  { Preserve, Generate };

// A mesh whose asset must be streamed in on a worker thread; the disk loader
// drains these after the entity pass and wires the handle in via callback.
struct PendingMesh { flecs::entity entity; std::string assetPath; };

struct SerdeContext {
    SerdeMode mode = SerdeMode::Memory;
    std::function<const Mesh*(MeshHandle)> meshLookup;   // save: handle -> sourcePath

    // Save: resolve sourcePath → cookedPath (optional; uses assetlib DB).
    // Returns empty string if the asset hasn't been cooked yet.
    std::function<std::string(const std::string& sourcePath)> cookedPathLookup;

    // Load: fast runtime path — loads cooked assets via AssetService.
    // When non-null and cookedPath is present, skips Assimp/glTF entirely.
    AssetService*             assetService = nullptr;

    ImporterRegistry*         importers   = nullptr;     // disk load: glTF (sync)
    AssetStorage*             storage      = nullptr;     // disk load: import target
    PrimitiveLibrary*         primitives   = nullptr;     // disk load: primitive shortcut
    std::vector<PendingMesh>* pendingAsync = nullptr;     // disk load: Assimp queue
};

struct ComponentSerde {
    const char* key;                                                   // json sub-key
    bool (*has)(flecs::entity);
    void (*save)(flecs::entity, nlohmann::json&, const SerdeContext&);  // writes its sub-object
    void (*load)(flecs::entity, const nlohmann::json&, SerdeContext&);  // reads its sub-object
};

// ── Transform ────────────────────────────────────────────────────────────────
inline bool hasTransform(flecs::entity e) { return e.try_get<Transform>() != nullptr; }
inline void saveTransform(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const Transform* t = e.try_get<Transform>(); if (!t) return;
    j["position"] = {t->position.x, t->position.y, t->position.z};
    j["rotation"] = {t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w};
    j["scale"]    = {t->scale.x, t->scale.y, t->scale.z};
}
inline void loadTransform(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    Transform t;
    if (j.contains("position")) t.position = {j["position"][0], j["position"][1], j["position"][2]};
    if (j.contains("rotation")) t.rotation = {j["rotation"][0], j["rotation"][1], j["rotation"][2], j["rotation"][3]};
    if (j.contains("scale"))    t.scale    = {j["scale"][0], j["scale"][1], j["scale"][2]};
    e.set<Transform>(t);
}

// ── Name (sub-json is a bare string) ─────────────────────────────────────────
inline bool hasName(flecs::entity e) { return e.try_get<Name>() != nullptr; }
inline void saveName(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const Name* n = e.try_get<Name>(); if (n) j = n->value;
}
inline void loadName(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    e.set<Name>({ j.is_string() ? j.get<std::string>() : std::string("Entity") });
}

// ── Camera ────────────────────────────────────────────────────────────────────
inline bool hasCamera(flecs::entity e) { return e.try_get<Camera>() != nullptr; }
inline void saveCamera(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const Camera* c = e.try_get<Camera>(); if (!c) return;
    j["isPrimary"]  = c->isPrimary;
    j["projection"] = (int)c->projection;
    j["fov"]        = c->fov;
    j["orthoSize"]  = c->orthoSize;
    j["nearPlane"]  = c->nearPlane;
    j["farPlane"]   = c->farPlane;
    j["clearColor"] = {c->clearColor[0], c->clearColor[1], c->clearColor[2], c->clearColor[3]};
}
inline void loadCamera(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    Camera c;
    c.isPrimary  = j.value("isPrimary", true);
    c.projection = (ProjectionType)j.value("projection", 0);
    c.fov        = j.value("fov", 60.0f);
    c.orthoSize  = j.value("orthoSize", 10.0f);
    c.nearPlane  = j.value("nearPlane", 0.1f);
    c.farPlane   = j.value("farPlane", 1000.0f);
    if (j.contains("clearColor")) {
        const auto& cc = j["clearColor"];
        c.clearColor[0]=cc[0]; c.clearColor[1]=cc[1]; c.clearColor[2]=cc[2]; c.clearColor[3]=cc[3];
    }
    e.set<Camera>(c);
}

// ── RigidBody (incl. collider dims that the old undo snapshot dropped) ────────
inline bool hasRigidBody(flecs::entity e) { return e.try_get<RigidBody>() != nullptr; }
inline void saveRigidBody(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const RigidBody* rb = e.try_get<RigidBody>(); if (!rb) return;
    j["bodyType"]    = (int)rb->bodyType;
    j["shape"]       = (int)rb->shape;
    j["mass"]        = rb->mass;
    j["restitution"] = rb->restitution;
    j["friction"]    = rb->friction;
    j["useGravity"]  = rb->useGravity;
    j["halfExtent"]  = {rb->halfExtent.x, rb->halfExtent.y, rb->halfExtent.z};
    j["radius"]      = rb->radius;
    j["halfHeight"]  = rb->halfHeight;
}
inline void loadRigidBody(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    RigidBody rb;
    rb.bodyType    = (PhysicsBodyType)j.value("bodyType", 1);
    rb.shape       = (PhysicsShape)   j.value("shape", 0);
    rb.mass        = j.value("mass", 1.0f);
    rb.restitution = j.value("restitution", 0.3f);
    rb.friction    = j.value("friction", 0.6f);
    rb.useGravity  = j.value("useGravity", true);
    if (j.contains("halfExtent")) { const auto& he = j["halfExtent"]; rb.halfExtent = {he[0], he[1], he[2]}; }
    rb.radius     = j.value("radius", 0.5f);
    rb.halfHeight = j.value("halfHeight", 0.5f);
    e.set<RigidBody>(rb);
}

// ── ScriptComponent (path only; instanceId/started are runtime-owned) ─────────
inline bool hasScript(flecs::entity e) {
    const ScriptComponent* s = e.try_get<ScriptComponent>();
    return s && !s->scriptPath.empty();
}
inline void saveScript(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const ScriptComponent* s = e.try_get<ScriptComponent>(); if (!s) return;
    j["path"] = s->scriptPath;
}
inline void loadScript(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    ScriptComponent sc; sc.scriptPath = j.value("path", std::string{});
    e.set<ScriptComponent>(sc);
}

// ── MeshRenderer (mode-aware: the one component that differs disk vs memory) ──
inline bool hasMesh(flecs::entity e) { return e.try_get<MeshRenderer>() != nullptr; }
inline void saveMesh(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const MeshRenderer* mr = e.try_get<MeshRenderer>(); if (!mr) return;
    const Mesh* mesh = ctx.meshLookup ? ctx.meshLookup(mr->mesh) : nullptr;
    if (ctx.mode == SerdeMode::Memory) {
        j["handleId"] = mr->mesh.id;
        if (mr->materialOverride.valid()) j["matOverrideId"] = mr->materialOverride.id;
        if (mesh && !mesh->sourcePath.empty()) j["sourcePath"] = mesh->sourcePath;
    } else { // Disk: asset path is the only cross-session-stable reference
        if (mesh && !mesh->sourcePath.empty()) {
            j["sourcePath"] = mesh->sourcePath;
            if (mesh->sourcePath.rfind("engine://primitive/", 0) == 0)
                j["sourceType"] = "primitive";
            // Also persist the cooked path for fast runtime loading.
            // Editor scenes carry both: sourcePath for re-import, cookedPath
            // for runtime. If the asset hasn't been cooked yet, skip it —
            // the legacy sourcePath import path still works.
            else if (ctx.cookedPathLookup) {
                std::string cooked = ctx.cookedPathLookup(mesh->sourcePath);
                if (!cooked.empty())
                    j["cookedPath"] = cooked;
            }
        }
        if (mr->materialOverride.valid()) j["matOverrideId"] = mr->materialOverride.id;
    }
}
inline void loadMesh(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    if (ctx.mode == SerdeMode::Memory) {
        uint32_t hid = j.value("handleId", 0u);
        if (hid > 0) {
            MeshHandle mh; mh.id = hid; MeshRenderer mr{mh};
            if (j.contains("matOverrideId")) mr.materialOverride.id = j["matOverrideId"].get<uint32_t>();
            e.set<MeshRenderer>(mr);
        }
        return;
    }

    // ── Fast path: cooked binary via AssetService (runtime) ────────────
    // If cookedPath is present and AssetService is available, load directly
    // from the cooked binary — no Assimp, no glTF import, no worker queue.
    std::string cookedPath = j.value("cookedPath", std::string{});
    if (!cookedPath.empty() && ctx.assetService) {
        MeshHandle h = ctx.assetService->loadMesh(cookedPath.c_str());
        if (h.valid()) {
            e.set<MeshRenderer>({h});
            return;
        }
        LOG_WARN("Scene", "Cooked mesh load failed, falling back to source: %s",
                 cookedPath.c_str());
    }

    // ── Legacy path: sourcePath → primitive / glTF / Assimp ────────────
    std::string srcPath = j.value("sourcePath", std::string{});
    std::string srcType = j.value("sourceType", std::string{});
    if (srcPath.empty()) return;

    bool isPrimitive = (srcType == "primitive") || (srcPath.rfind("engine://primitive/", 0) == 0);
    if (isPrimitive && ctx.primitives && ctx.primitives->ready()) {
        std::string primName = srcPath.substr(srcPath.rfind('/') + 1);
        MeshHandle h = ctx.primitives->byName(primName);
        if (h.valid()) { e.set<MeshRenderer>({h}); return; }
    }

    std::string ext = std::filesystem::path(srcPath).extension().string();
    for (auto& c : ext) c = (char)std::tolower(c);
    const bool isGltf = (ext == ".glb" || ext == ".gltf");
    if (isGltf && ctx.importers && ctx.storage) {
        auto result = ctx.importers->loadCached(srcPath, *ctx.storage);
        if (result.success) {
            if (Mesh* m = const_cast<Mesh*>(ctx.storage->meshes.getMesh(result.mesh)))
                m->sourcePath = srcPath;
            e.set<MeshRenderer>({result.mesh});
        } else {
            LOG_ERROR("Scene", "glTF load failed: %s", result.error.c_str());
        }
        return;
    }
    if (ctx.pendingAsync) ctx.pendingAsync->push_back({e, srcPath}); // Assimp on a worker
}

// ── The one table ─────────────────────────────────────────────────────────────
// ── CharacterController (capsule controller; 'grounded' is runtime-only) ──────
inline bool hasCharacterController(flecs::entity e) { return e.try_get<CharacterController>() != nullptr; }
inline void saveCharacterController(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const CharacterController* cc = e.try_get<CharacterController>(); if (!cc) return;
    j["radius"]       = cc->radius;
    j["height"]       = cc->height;
    j["maxSlopeDeg"]  = cc->maxSlopeDeg;
    j["stepHeight"]   = cc->stepHeight;
    j["mass"]         = cc->mass;
    j["gravityScale"] = cc->gravityScale;
}
inline void loadCharacterController(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    CharacterController cc;
    cc.radius       = j.value("radius", 0.3f);
    cc.height       = j.value("height", 1.8f);
    cc.maxSlopeDeg  = j.value("maxSlopeDeg", 45.0f);
    cc.stepHeight   = j.value("stepHeight", 0.3f);
    cc.mass         = j.value("mass", 70.0f);
    cc.gravityScale = j.value("gravityScale", 1.0f);
    e.set<CharacterController>(cc);
}

// ── Light (parameters only; world pos/dir derive from Transform at extraction) ─
inline bool hasLight(flecs::entity e) { return e.try_get<Light>() != nullptr; }
inline void saveLight(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const Light* l = e.try_get<Light>(); if (!l) return;
    j["type"]        = (int)l->type;
    j["color"]       = {l->color.x, l->color.y, l->color.z};
    j["intensity"]   = l->intensity;
    j["range"]       = l->range;
    j["spotInner"]   = l->spotInner;
    j["spotOuter"]   = l->spotOuter;
    j["castShadows"] = l->castShadows;
    j["useTemperature"] = l->useTemperature;
    j["temperatureK"]   = l->temperatureK;
}
inline void loadLight(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    Light l;
    l.type        = (LightType)j.value("type", 0);
    if (j.contains("color")) { const auto& c = j["color"]; l.color = {c[0], c[1], c[2]}; }
    l.intensity   = j.value("intensity", 3.0f);
    l.range       = j.value("range", 15.0f);
    l.spotInner   = j.value("spotInner", 25.0f);
    l.spotOuter   = j.value("spotOuter", 35.0f);
    l.castShadows = j.value("castShadows", false);
    l.useTemperature = j.value("useTemperature", false);
    l.temperatureK   = j.value("temperatureK", 6500.0f);
    e.set<Light>(l);
}

// ── SkinnedMesh (skeleton handle only; skin matrices are runtime) ────────────
inline bool hasSkinnedMesh(flecs::entity e) { return e.try_get<SkinnedMesh>() != nullptr; }
inline void saveSkinnedMesh(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const SkinnedMesh* sm = e.try_get<SkinnedMesh>(); if (!sm) return;
    if (ctx.mode == SerdeMode::Memory) {
        j["skeletonId"] = sm->skeleton.id;
    } else {
        // Disk: save skeleton handle id. Phase 6 will add a skeleton asset
        // path for cross-session stability; for now handle id is sufficient
        // since skeletons are re-imported alongside the mesh.
        j["skeletonId"] = sm->skeleton.id;
    }
}
inline void loadSkinnedMesh(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    SkinnedMesh sm;
    sm.skeleton.id = j.value("skeletonId", 0u);
    e.set<SkinnedMesh>(sm);
}

// ── Animator (playback state) ────────────────────────────────────────────────
inline bool hasAnimator(flecs::entity e) { return e.try_get<Animator>() != nullptr; }
inline void saveAnimator(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const Animator* a = e.try_get<Animator>(); if (!a) return;
    if (ctx.mode == SerdeMode::Memory) {
        j["clipId"] = a->clip.id;
    } else {
        j["clipId"] = a->clip.id;
    }
    j["time"]    = a->time;
    j["speed"]   = a->speed;
    j["playing"] = a->playing;
    j["looping"] = a->looping;
}
inline void loadAnimator(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    Animator a;
    a.clip.id = j.value("clipId", 0u);
    a.time    = j.value("time", 0.0f);
    a.speed   = j.value("speed", 1.0f);
    a.playing = j.value("playing", false);
    a.looping = j.value("looping", true);
    e.set<Animator>(a);
}

inline const std::vector<ComponentSerde>& table() {
    static const std::vector<ComponentSerde> t = {
        { "transform",    hasTransform, saveTransform, loadTransform },
        { "name",         hasName,      saveName,      loadName      },
        { "meshRenderer", hasMesh,      saveMesh,      loadMesh      },
        { "camera",       hasCamera,    saveCamera,    loadCamera    },
        { "rigidBody",    hasRigidBody, saveRigidBody, loadRigidBody },
        { "script",       hasScript,    saveScript,    loadScript    },
        { "characterController", hasCharacterController, saveCharacterController, loadCharacterController },
        { "light",        hasLight,      saveLight,     loadLight     },
        { "skinnedMesh",  hasSkinnedMesh, saveSkinnedMesh, loadSkinnedMesh },
        { "animator",     hasAnimator,    saveAnimator,    loadAnimator    },
    };
    return t;
}

// ── Primitives every path shares ──────────────────────────────────────────────
// Caller must have run assignMissingIds() first (saveEntity reads ids, never
// assigns — it may run inside a flecs query iteration where set<> is illegal).
inline nlohmann::json saveEntity(flecs::entity e, const SerdeContext& ctx) {
    nlohmann::json je;
    if (const EntityId* id = e.try_get<EntityId>()) je["id"] = id->value;
    flecs::entity par = e.target(flecs::ChildOf);
    if (par && par.is_alive())
        if (const EntityId* pid = par.try_get<EntityId>()) je["parentId"] = pid->value;
    for (const auto& d : table())
        if (d.has(e)) { nlohmann::json sub; d.save(e, sub, ctx); je[d.key] = std::move(sub); }
    return je;
}

// Creates an anonymous entity (no flecs name path -> no duplicate-name
// aliasing), resolves identity per policy, then drives every table load.
// Parent links are NOT restored here. Must be called outside query iteration.
inline flecs::entity createEntity(flecs::world& w, const nlohmann::json& je,
                                  SerdeContext& ctx, IdPolicy policy) {
    flecs::entity e = w.entity();                       // anonymous
    uint64_t id = (policy == IdPolicy::Preserve) ? je.value("id", (uint64_t)0) : 0;
    if (id == 0 || findById(w, id).is_alive()) {        // dedupe: never silently alias
        if (id != 0) LOG_WARN("Scene", "EntityId %llu already live — reassigning",
                              (unsigned long long)id);
        do { id = generateEntityId(); } while (findById(w, id).is_alive());
    }
    e.set<EntityId>({id});
    for (const auto& d : table())
        if (je.contains(d.key)) d.load(e, je[d.key], ctx);
    return e;
}

} // namespace EntitySerde
