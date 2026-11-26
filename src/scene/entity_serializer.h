#pragma once
// ── EntitySerializer ─────────────────────────────────────────────────────────
// The single component (de)serialization path. Scene save/load, the in-memory
// editor->game snapshot, and the undo stack all walk the same schema registry
// (scene/component_schema.h), so a component serialized in one place can never
// drift from another. Register a component once (builtinSchemas() below, or a
// provider's own registration) and every path picks it up.
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
#include <algorithm>
#include <cmath>

#include "core/transform.h"
#include "core/handle.h"
#include "components/entity_id_util.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/colour_grading.h"
#include "components/light.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/skinned_mesh.h"
#include "components/lod_mesh.h"
#include "components/mesh_placeholder.h"
#include "components/animator.h"
#include "render/asset_registry.h"
#include "assets/asset_ref.h"
#include "assets/asset_storage.h"
#include "render/primitive_library.h"
#include "scene/scene_assets.h"
#include "core/lod_limit.h"
#include "assets/cooked_scene.h"
#include "scene/component_schema.h"
#include "scene/reflected_serde.h"
#include "scene/unknown_components.h"
#include "scene/unresolved_mesh.h"
#include "core/logger.h"
#include "core/json_read.h"

namespace EntitySerde {

// Disk: meshes referenced by asset sourcePath, re-resolved on load (possibly
// async). Memory: live handle ids reused directly (instant, same-session).
enum class SerdeMode { Disk, Memory };
enum class IdPolicy  { Preserve, Generate };

// A mesh whose asset must be streamed in on a worker thread; the disk loader
// drains these after the entity pass and wires the handle in via callback.
// `material` is the authored override, already resolved: the callback applies
// it with the mesh, or a streamed mesh would lose it (BUG-0072).
struct PendingMesh { flecs::entity entity; std::string assetPath; MaterialHandle material; };

struct SerdeContext {
    SerdeMode mode = SerdeMode::Memory;
    std::function<const Mesh*(MeshHandle)> meshLookup;   // save: handle -> sourcePath

    // Asset identity (disk mode): refs serialize as UUID + project-relative
    // path (AssetRef) and resolve back through the registry on load.
    std::filesystem::path    projectRoot;
    assetlib::AssetRegistry* assetLib = nullptr;

    // Save: resolve sourcePath → cookedPath (optional; uses assetlib DB).
    // Returns empty string if the asset hasn't been cooked yet.
    std::function<std::string(const std::string& sourcePath)> cookedPathLookup;

    // Load: the host's asset hooks (scene/scene_assets.h). With loadCookedMesh
    // set and a cookedPath present, skips Assimp/glTF entirely. Scene never
    // includes the runtime; the runtime fills these in (WO-047).
    const SceneAssets*        sceneAssets = nullptr;

    // Save (disk): MaterialHandle -> the material's AUTHORED NAME. A handle is
    // a session-local slot index; writing one to disk is meaningless, which is
    // what the old `matOverrideId` field did. Empty when the host has none.
    std::function<std::string(MaterialHandle)> materialNameLookup;


    // Load: id-collision index. Optional, but a loader creating many entities
    // MUST set it — createEntity's fallback is findById, which is O(n) and made
    // scene load quadratic (97.6% of a 22 s load over 50 000 entities). One-off
    // callers like undo can leave it null.
    EntityIdIndex*            idIndex     = nullptr;
    AssetStorage*             storage      = nullptr;     // disk load: import target
    PrimitiveLibrary*         primitives   = nullptr;     // disk load: primitive shortcut
    std::vector<PendingMesh>* pendingAsync = nullptr;     // disk load: Assimp queue

    // The schemas to read and write with. Null: the engine's own (schemas()). A
    // test passes its own registry to load data under a revision it chose.
    const SchemaRegistry*     schemas      = nullptr;

    // Load: entity-reference fields waiting for the entity they name. A loader
    // creating many entities sets this and calls resolveEntityRefs once they all
    // exist; without it (undo restoring one entity) a reference resolves at once,
    // against the entities already in the world.
    struct EntityRef {
        flecs::entity owner;
        uint64_t      stableId;
        std::function<void(flecs::entity owner, flecs::entity target)> apply;
    };
    std::vector<EntityRef>*   entityRefs   = nullptr;

    // Load: set when a mesh this entity loaded brought a skin, so finishing the
    // entity looks for one only then (a component lookup per entity, at scene
    // scale, was measurable: WO-092).
    bool                      skinPending  = false;
};

inline uint64_t stableIdOf(flecs::entity target) {
    if (!target.is_valid() || !target.is_alive()) return 0;
    const EntityId* id = target.try_get<EntityId>();
    return id ? id->value : 0;
}

// Resolves one reference: through the id index when the loader has one (O(1)),
// else by a scan, which only the one-entity paths (undo) take.
inline void applyEntityRef(flecs::world& w, const SerdeContext::EntityRef& r,
                           const EntityIdIndex* index) {
    flecs::entity target;
    if (index) {
        if (auto it = index->map.find(r.stableId); it != index->map.end()) target = it->second;
    } else {
        target = findById(w, r.stableId);
    }
    if (!target.is_alive()) {
        LOG_WARN("Scene", "entity reference to %llu: no such entity here — loaded as none",
                 (unsigned long long)r.stableId);
        return;
    }
    if (r.owner.is_alive()) r.apply(r.owner, target);
}

inline void deferEntityRef(SerdeContext& ctx, flecs::entity owner, uint64_t stableId,
                           std::function<void(flecs::entity, flecs::entity)> apply) {
    if (ctx.entityRefs) ctx.entityRefs->push_back({owner, stableId, std::move(apply)});
}

// The references of a load that did not bring its own list: a single component
// (undo restoring one) or a single entity. The outermost load to start owns the
// list and resolves it when it is done, after its components are set.
inline bool beginEntityRefs(SerdeContext& ctx) {
    if (ctx.entityRefs) return false;
    static thread_local std::vector<SerdeContext::EntityRef> local;
    local.clear();
    ctx.entityRefs = &local;
    return true;
}
inline void endEntityRefs(SerdeContext& ctx, flecs::world w, bool owned) {
    if (!owned) return;
    std::vector<SerdeContext::EntityRef>* refs = ctx.entityRefs;
    ctx.entityRefs = nullptr;
    for (const SerdeContext::EntityRef& r : *refs) applyEntityRef(w, r, ctx.idIndex);
    refs->clear();
}

// Every deferred reference, once the scene's entities all exist.
inline void resolveEntityRefs(flecs::world& w, SerdeContext& ctx) {
    if (!ctx.entityRefs) return;
    for (const SerdeContext::EntityRef& r : *ctx.entityRefs) applyEntityRef(w, r, ctx.idIndex);
    ctx.entityRefs->clear();
}

// Bounds- and type-safe JSON reads live in core/json_read.h — the same defect
// was found in four files, so the helper is shared rather than copied a fifth
// time. See that header for what nlohmann's const operator[] actually does.
using jsonread::finiteNumber;
using jsonread::readFloat;
using jsonread::readFloats;

// ── Name (sub-json is a bare string) ─────────────────────────────────────────
inline bool hasName(flecs::entity e) { return e.try_get<Name>() != nullptr; }
inline void saveName(flecs::entity e, nlohmann::json& j, const SerdeContext&) {
    const Name* n = e.try_get<Name>(); if (n) j = n->value;
}
inline void loadName(flecs::entity e, const nlohmann::json& j, SerdeContext&) {
    e.set<Name>({ j.is_string() ? j.get<std::string>() : std::string("Entity") });
}

// ── A skinned mesh's skeleton and clips, wired once the entity is loaded ─────
// Handles are session-local: the scene stores identity only (Animator::clipPath,
// or the mesh's embedded clipIndex), and the handles come from THIS load. The
// mesh brings the skeleton, but the entity's SkinnedMesh and Animator are read
// after it (registry order), so the wiring waits for the end of the entity
// (finishSkin) and starts from the Animator as authored. The editor's import
// callback (scene_serializer.h) wires the same way, through the same function.
struct PendingSkin {
    SkeletonHandle              skeleton;
    std::vector<AnimClipHandle> clips;
};

// A skinned mesh without an authored Animator plays its clip, which is what the
// editor has always shown; one with an Animator keeps its `playing`.
inline void wireSkin(flecs::entity e, SkeletonHandle skeleton,
                     const std::vector<AnimClipHandle>& clips,
                     const std::function<AnimClipHandle(const std::string&, SkeletonHandle)>& bindClip) {
    if (!skeleton.valid()) return;
    SkinnedMesh sm;
    sm.skeleton = skeleton;
    e.set<SkinnedMesh>(sm);

    Animator anim;
    bool hadAnimator = false;
    if (const Animator* a = e.try_get<Animator>()) { anim = *a; hadAnimator = true; }
    if (!anim.clipPath.empty() && bindClip) {
        // A standalone clip asset, bound by bone name to this skeleton.
        const AnimClipHandle h = bindClip(anim.clipPath, skeleton);
        if (h.valid()) {
            anim.clip = h;
            if (!hadAnimator) anim.playing = true;
        }
    } else if (!clips.empty()) {   // a clip inside the mesh file
        int idx = anim.clipIndex;
        if (idx < 0 || idx >= (int)clips.size()) idx = 0;
        anim.clipIndex = idx;
        anim.clip      = clips[idx];
        if (!hadAnimator) anim.playing = true;
    }
    e.set<Animator>(anim);
}

// ── MeshRenderer (mode-aware: the one component that differs disk vs memory) ──
// An entity whose authored mesh did not resolve still HAS a mesh as far as the
// document is concerned — see scene/unresolved_mesh.h (WO-029).
inline bool hasMesh(flecs::entity e) {
    return e.try_get<MeshRenderer>() != nullptr || e.try_get<UnresolvedMesh>() != nullptr;
}
inline void saveMesh(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const MeshRenderer* mr = e.try_get<MeshRenderer>();
    // Showing the placeholder (WO-018): the MeshRenderer is a stand-in, and
    // what is saved is the authored reference, as if there were none.
    const MeshPlaceholder* ph = e.try_get<MeshPlaceholder>();
    const bool placeholder = mr && ph && ph->shown == mr->mesh && e.has<UnresolvedMesh>();
    if (!mr || placeholder) {
        // Not resolved: write back exactly what was authored. Disk gets the
        // original object; a memory snapshot keeps it under "unresolved" so a
        // restore (undo, end of Play) puts the same component back.
        const UnresolvedMesh* u = e.try_get<UnresolvedMesh>(); if (!u) return;
        auto authored = nlohmann::json::parse(u->json, nullptr, false);
        if (authored.is_discarded()) return;   // cannot happen: we wrote it with dump()
        if (ctx.mode == SerdeMode::Disk) {
            j = std::move(authored);
        } else {
            j["unresolved"]       = std::move(authored);
            j["unresolvedReason"] = u->reason;
            if (placeholder) j["placeholderId"] = mr->mesh.id;
        }
        return;
    }
    const Mesh* mesh = ctx.meshLookup ? ctx.meshLookup(mr->mesh) : nullptr;
    if (ctx.mode == SerdeMode::Memory) {
        j["handleId"] = mr->mesh.id;
        if (mr->materialOverride.valid()) j["matOverrideId"] = mr->materialOverride.id;
        if (mesh && !mesh->sourcePath.empty()) j["sourcePath"] = mesh->sourcePath;
    } else { // Disk: UUID + project-relative path (AssetRef) — never absolute
        if (mesh && !mesh->sourcePath.empty()) {
            AssetRef ref = assetref::make(mesh->sourcePath, ctx.projectRoot,
                                          ctx.assetLib);
            assetref::toJson(ref, j);
            if (mesh->sourcePath.rfind("engine://primitive/", 0) == 0)
                j["sourceType"] = "primitive";
            // Also persist the cooked path for fast runtime loading.
            // If the asset hasn't been cooked yet, skip it — the source
            // import path still works.
            else if (ctx.cookedPathLookup) {
                std::string cooked = ctx.cookedPathLookup(mesh->sourcePath);
                if (!cooked.empty())
                    j["cookedPath"] = cooked;
            }
        }
        // Disk: the material's NAME. Never the handle id — see
        // SerdeContext::materialNameLookup.
        if (mr->materialOverride.valid() && ctx.materialNameLookup) {
            const std::string name = ctx.materialNameLookup(mr->materialOverride);
            if (!name.empty()) j["material"] = name;
        }
    }
}
inline void loadMeshMemory(flecs::entity e, const nlohmann::json& j) {
    {
        if (auto it = j.find("unresolved"); it != j.end() && it->is_object()) {
            e.set<UnresolvedMesh>({jsonread::dumpLoaded(*it), j.value("unresolvedReason", std::string{}), false});
            if (const uint32_t pid = j.value("placeholderId", 0u)) {
                MeshHandle mh; mh.id = pid;
                e.set<MeshRenderer>({mh});
                e.set<MeshPlaceholder>({mh});
            }
            return;
        }
        uint32_t hid = j.value("handleId", 0u);
        if (hid > 0) {
            MeshHandle mh; mh.id = hid; MeshRenderer mr{mh};
            if (j.contains("matOverrideId")) mr.materialOverride.id = j["matOverrideId"].get<uint32_t>();
            e.set<MeshRenderer>(mr);
        }
        return;
    }

}

// A string field of a mesh reference as nlohmann's `value(key, "")` reads it:
// absent is empty, a string is its value, anything else (or a reference that is
// not an object) THROWS, which skips the component and keeps the reference.
// Written over the value interface, so the cooked scene's CBOR is read the same.
template <class V>
inline std::string stringField(const V& j, const char* key) {
    if (!j.isObject()) throw std::runtime_error("a mesh reference is not an object");
    V v;
    if (!j.find(key, v)) return {};
    if (!v.isString()) throw std::runtime_error(std::string("\"") + key + "\" is not a string");
    return v.asString();
}

// Disk: a mesh by its cooked path, else its source reference. `authored()` is
// the reference as JSON text, asked for only when the mesh does not resolve.
template <class V, class Dump>
inline void loadMeshDisk(flecs::entity e, const V& j, const Dump& authored, SerdeContext& ctx,
                         bool& didResolve) {
    // ── Fast path: cooked binary via the host (SceneAssets) ────────────
    // If cookedPath is present and the host can load cooked meshes, load directly
    // from the cooked binary — no Assimp, no glTF import, no worker queue.
    //
    // PASSED RELATIVE, DELIBERATELY. `cookedPath` comes from the registry as
    // "meshs/<uuid>.cooked", which is relative to the project's .cache — and
    // AssetService::loadMesh resolves relative paths against exactly that. This
    // used to pre-join ctx.projectRoot, producing "<project>/meshs/<uuid>.cooked",
    // a path that has never existed. Every cooked mesh load therefore failed and
    // fell through to the Assimp source importer below, which is why nothing
    // looked broken: scenes still rendered, just via the slow path, and a shipped
    // dist with no source assets would have failed outright.
    // Keep the authored reference until something resolves it (WO-029). Every
    // path below that does not set a MeshRenderer calls unresolved(), which
    // keeps it with the reason, so the next save cannot drop the mesh; loadMesh
    // keeps it too if a read throws partway. Kept lazily: most meshes resolve on
    // the cooked path, and writing the reference only to remove it again was a
    // string dump and two archetype moves per mesh (WO-092).
    auto unresolved = [&e, &authored](std::string why, bool pending = false) {
        if (!e.has<UnresolvedMesh>()) e.set<UnresolvedMesh>({authored(), "", false});
        UnresolvedMesh& u = e.get_mut<UnresolvedMesh>();
        u.reason = std::move(why); u.pending = pending;
    };
    auto resolved = [&e, &didResolve](MeshRenderer mr) {
        didResolve = true;
        e.set<MeshRenderer>(mr);
        // Checked first: removing a component the entity never had still costs
        // flecs a lookup, and at scene scale it showed (WO-092).
        if (e.has<UnresolvedMesh>())  e.remove<UnresolvedMesh>();
        if (e.has<MeshPlaceholder>()) e.remove<MeshPlaceholder>();
    };

    // The authored material name, resolved to a handle. Empty is normal — most
    // entities use the material baked into their mesh.
    MaterialHandle matOverride;
    const SceneAssets* sa = ctx.sceneAssets;
    if (sa && sa->loadMaterial) {
        const std::string matName = stringField(j, "material");
        if (!matName.empty()) {
            matOverride = sa->loadMaterial(matName.c_str());
            if (!matOverride.valid())
                LOG_WARN("Scene", "material \"%s\" not found — the mesh's own "
                         "material is used instead", matName.c_str());
        }
    }

    const std::string cookedPath = stringField(j, "cookedPath");
    if (!cookedPath.empty() && sa && sa->loadCookedMesh) {
        // Request the cooked LOD chain too. Without this the editor's load path
        // silently produced no levels while scene_service's did — the same
        // asset rendering with LOD in the player and without it in the editor.
        //
        // And the skin: before this the editor's cooked path asked for none, so a
        // skinned mesh loaded this way kept an invalid skeleton and never animated
        // (WO-091); only the import path and the player's own loader wired one.
        std::vector<MeshHandle> lods;
        StreamedMesh skin;
        MeshHandle h = sa->loadCookedMesh(cookedPath.c_str(), &lods, &skin);
        if (h.valid()) {
            resolved({h, matOverride});
            if (skin.skeleton.valid()) {
                e.set<PendingSkin>({skin.skeleton, skin.clips});
                ctx.skinPending = true;
            }
            if (!lods.empty()) {
                LodMesh lm;
                lm.count = (uint8_t)std::min(lods.size(),
                                             (size_t)(::kMaxLodLevels - 1));
                for (uint8_t i = 0; i < lm.count; ++i) lm.mesh[i] = lods[i];
                e.set<LodMesh>(lm);
            }
            return;
        }
        LOG_WARN("Scene", "Cooked mesh load failed, falling back to source: %s",
                 cookedPath.c_str());
    }

    // ── Source path: AssetRef (uuid + relative) or legacy absolute ─────
    // UUID resolves through the registry (survives renames/moves); relative
    // path resolves against the project root; legacy absolute paths are
    // accepted as-is and re-rooted by assets/ suffix when stale.
    AssetRef ref;   // as assetref::fromJson reads it
    ref.uuid = stringField(j, "asset");
    ref.path = stringField(j, "path");
    if (ref.path.empty()) ref.path = stringField(j, "sourcePath");   // legacy (absolute)
    std::string srcPath = assetref::resolve(ref, ctx.projectRoot, ctx.assetLib);
    std::string srcType = stringField(j, "sourceType");
    if (srcPath.empty()) {
        if (!ref.empty()) {
            LOG_WARN("Scene", "Mesh reference unresolved (uuid=%s path=%s) — kept, "
                     "and saved back unchanged", ref.uuid.c_str(), ref.path.c_str());
            unresolved("source not found");
        } else if (cookedPath.empty()) {
            e.remove<UnresolvedMesh>();   // nothing was authored; nothing to keep
        } else {
            unresolved("cooked load failed and there is no source reference");
        }
        return;
    }

    bool isPrimitive = (srcType == "primitive") || (srcPath.rfind("engine://primitive/", 0) == 0);
    if (isPrimitive && ctx.primitives && ctx.primitives->ready()) {
        std::string primName = srcPath.substr(srcPath.rfind('/') + 1);
        MeshHandle h = ctx.primitives->byName(primName);
        if (h.valid()) { resolved({h, matOverride}); return; }
    }

    // Everything else is loaded by SOURCE path from its cooked version, off
    // the calling thread (SceneAssets::streamMesh). There is no in-process
    // parse of any format any more, glTF included (WO-018): the one path for
    // an uncooked asset is a cook, so it looks the same whether or not it was
    // cooked before the scene opened.
    if (ctx.pendingAsync) {
        ctx.pendingAsync->push_back({e, srcPath, matOverride});
        unresolved("loading", /*pending*/ true);    // the callback resolves it
    } else {
        LOG_WARN("Scene", "nothing loads source-referenced meshes here: %s — reference kept",
                 srcPath.c_str());
        unresolved("no loader in this host");
    }
}

// Disk load, then the placeholder (WO-018): an entity whose mesh did not
// resolve here (Pending: a cook is coming; Failed: none will) SHOWS the
// primitive cube instead of nothing. MeshPlaceholder records that the
// MeshRenderer is the stand-in, so saveMesh still writes the authored
// reference. Needs the primitive library (absent headless: no placeholder).
// Disk load, then the placeholder (WO-018): an entity whose mesh did not
// resolve here (Pending: a cook is coming; Failed: none will) SHOWS the
// primitive cube instead of nothing. MeshPlaceholder records that the
// MeshRenderer is the stand-in, so saveMesh still writes the authored
// reference. Needs the primitive library (absent headless: no placeholder).
template <class V, class Dump>
inline void loadMeshFrom(flecs::entity e, const V& j, const Dump& authored, SerdeContext& ctx) {
    bool didResolve = false;
    try {
        loadMeshDisk(e, j, authored, ctx, didResolve);
    } catch (...) {
        // A malformed reference that threw partway is still the author's: keep
        // it, so a save writes it back (scene_mesh_reference_test, case 6).
        if (!e.has<MeshRenderer>() && !e.has<UnresolvedMesh>())
            e.set<UnresolvedMesh>({authored(), "malformed", false});
        throw;
    }
    if (didResolve) return;
    if (!ctx.primitives || !ctx.primitives->ready()) return;
    if (!e.has<UnresolvedMesh>() || e.has<MeshRenderer>()) return;
    const MeshHandle cube = ctx.primitives->cube();
    e.set<MeshRenderer>({cube});
    e.set<MeshPlaceholder>({cube});
}

inline void loadMesh(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    if (ctx.mode == SerdeMode::Memory) { loadMeshMemory(e, j); return; }
    loadMeshFrom(e, JsonValue{&j}, [&j] { return jsonread::dumpLoaded(j); }, ctx);
}

// ── LodMesh (the coarser levels; level 0 is MeshRenderer's own mesh) ──────────
//
// LEVELS RESOLVE SYNCHRONOUSLY, AND ONLY FROM A COOKED MESH (or a live handle in
// Memory mode). That is a real limitation, stated rather than hidden: loadMesh's
// last resort is `ctx.pendingAsync`, an Assimp import on a worker that completes
// later by setting a MeshRenderer on the entity. There is no shape of that
// mechanism that can fill slot 2 of an LodMesh, so rather than half-wire it, a
// level that is not cooked is DROPPED and the chain is shortened.
//
// Shortening is safe by construction, not by luck: `coarsenBelow` is indexed from
// the fine end, so dropping the tail removes the coarsest levels and the remaining
// thresholds keep their meaning. And LOD meshes are cooker output — a decimated
// level has no hand-authored source to import — so the cooked path is the one that
// will actually be exercised. A dropped level costs triangles, never correctness;
// render/world/lod.h's selection walks back to finer levels for the same reason.
inline bool hasLodMesh(flecs::entity e) { return e.try_get<LodMesh>() != nullptr; }
inline void saveLodMesh(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const LodMesh* lm = e.try_get<LodMesh>(); if (!lm || lm->count == 0) return;
    nlohmann::json levels = nlohmann::json::array();
    for (uint8_t i = 0; i < lm->count && i < ::kMaxLodLevels - 1; ++i) {
        nlohmann::json lj = nlohmann::json::object();
        const Mesh* mesh = ctx.meshLookup ? ctx.meshLookup(lm->mesh[i]) : nullptr;
        if (ctx.mode == SerdeMode::Memory) {
            lj["handleId"] = lm->mesh[i].id;
        } else if (mesh && !mesh->sourcePath.empty() && ctx.cookedPathLookup) {
            std::string cooked = ctx.cookedPathLookup(mesh->sourcePath);
            if (cooked.empty()) continue;      // uncooked: drop, see the note above
            lj["cookedPath"] = cooked;
            // The source ref rides along so the level survives a re-cook that
            // changes the cooked filename — resolution prefers cookedPath.
            assetref::toJson(assetref::make(mesh->sourcePath, ctx.projectRoot,
                                            ctx.assetLib), lj);
        } else {
            continue;
        }
        levels.push_back(std::move(lj));
    }
    if (levels.empty()) return;               // nothing survived: omit entirely
    // Only the thresholds the surviving levels use. Writing all three when one
    // level exists would round-trip into a chain that claims levels it lacks.
    nlohmann::json below = nlohmann::json::array();
    for (std::size_t i = 0; i < levels.size(); ++i) below.push_back(lm->coarsenBelow[i]);
    j["levels"]       = std::move(levels);
    j["coarsenBelow"] = std::move(below);
}
inline void loadLodMesh(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    if (!j.contains("levels") || !j["levels"].is_array()) return;
    LodMesh lm;
    const auto& levels = j["levels"];
    const bool  hasBelow = j.contains("coarsenBelow") && j["coarsenBelow"].is_array();
    for (const auto& lj : levels) {
        if (lm.count >= ::kMaxLodLevels - 1) break;
        MeshHandle h;
        if (ctx.mode == SerdeMode::Memory) {
            h.id = lj.value("handleId", 0u);
        } else {
            const std::string cooked = lj.value("cookedPath", std::string{});
            if (!cooked.empty() && ctx.sceneAssets && ctx.sceneAssets->loadCookedMesh)
                h = ctx.sceneAssets->loadCookedMesh(cooked.c_str(), nullptr, nullptr);   // relative — see above
        }
        if (!h.valid()) {
            LOG_WARN("Scene", "LOD level %u unresolved, chain shortened",
                     (unsigned)lm.count + 1);
            break;   // BREAK, not continue: a gap would shift every coarser level
                     // one threshold finer, silently changing when it appears.
        }
        // Read the threshold at the SOURCE index, so a shortened chain still uses
        // the thresholds that were authored for the levels that survived.
        const std::size_t src = (std::size_t)lm.count;
        if (hasBelow && src < j["coarsenBelow"].size())
            lm.coarsenBelow[lm.count] = j["coarsenBelow"][src].get<float>();
        lm.mesh[lm.count] = h;
        ++lm.count;
    }
    if (lm.count) e.set<LodMesh>(lm);
}

// ── SkinnedMesh (skeleton handle is session-local — NEVER hits disk) ─────────
// On disk the component is a presence marker: the skeleton always comes from
// the entity's mesh source file, and the async import callback wires the
// fresh handle in. Memory mode (play snapshot) reuses the live handle.
inline bool hasSkinnedMesh(flecs::entity e) { return e.try_get<SkinnedMesh>() != nullptr; }
inline void saveSkinnedMesh(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const SkinnedMesh* sm = e.try_get<SkinnedMesh>(); if (!sm) return;
    if (ctx.mode == SerdeMode::Memory)
        j["skeletonId"] = sm->skeleton.id;     // same-session handle reuse
    else
        j = nlohmann::json::object();          // presence marker only
}
inline void loadSkinnedMesh(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    SkinnedMesh sm;
    if (ctx.mode == SerdeMode::Memory)
        sm.skeleton.id = j.value("skeletonId", 0u);
    // Disk: handle stays invalid — the import callback resolves it.
    // (Legacy "skeletonId" in old scene files is deliberately ignored:
    // it was a load-order-dependent slot index, not an identity.)
    e.set<SkinnedMesh>(sm);
}

// ── Animator (playback state + semantic clip selection) ──────────────────────
// The clip is identified by its index within the mesh's source file
// (clipIndex) — stable across sessions. The runtime handle is session-local:
// serialized only in Memory mode, resolved by the import callback on disk load.
inline bool hasAnimator(flecs::entity e) { return e.try_get<Animator>() != nullptr; }
inline void saveAnimator(flecs::entity e, nlohmann::json& j, const SerdeContext& ctx) {
    const Animator* a = e.try_get<Animator>(); if (!a) return;
    if (ctx.mode == SerdeMode::Memory) {
        j["clipId"]   = a->clip.id;            // same-session handle reuse
        if (!a->clipPath.empty()) j["clipPath"] = a->clipPath;  // raw runtime path
    } else if (!a->clipPath.empty()) {
        // Standalone clip asset: uuid + project-relative path ("asset"/"path"),
        // same identity scheme meshes use — survives sessions and renames.
        AssetRef ref = assetref::make(a->clipPath, ctx.projectRoot, ctx.assetLib);
        assetref::toJson(ref, j);
    }
    j["clipIndex"] = a->clipIndex;
    j["fade"]    = a->fade;
    j["time"]    = a->time;
    j["speed"]   = a->speed;
    j["playing"] = a->playing;
    j["looping"] = a->looping;
}
inline void loadAnimator(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    Animator a;
    if (ctx.mode == SerdeMode::Memory) {
        a.clip.id  = j.value("clipId", 0u);
        a.clipPath = j.value("clipPath", std::string{});
    } else {
        // Disk: resolve the standalone-clip AssetRef to a loadable source
        // path; the import callback binds it to the skeleton once the skinned
        // mesh arrives. Clip handle stays invalid until then.
        //
        // A reference that does not resolve HERE keeps its authored path. The
        // shipped player has no source assets (the clip library maps the path to
        // the packaged cooked clip), and an editor missing the file must still
        // save the reference back rather than erase it (DR-0034).
        AssetRef ref = assetref::fromJson(j);
        if (!ref.empty()) {
            a.clipPath = assetref::resolve(ref, ctx.projectRoot, ctx.assetLib);
            if (a.clipPath.empty()) a.clipPath = ref.path;
        }
    }
    a.clipIndex = j.value("clipIndex", 0);
    a.fade    = readFloat(j, "fade", 0.2f);
    a.time    = readFloat(j, "time", 0.0f);
    a.speed   = readFloat(j, "speed", 1.0f);
    a.playing = j.value("playing", false);
    a.looping = j.value("looping", true);
    e.set<Animator>(a);
}

// A value of the cooked scene's CBOR as JSON: for what has no in-place reader.
inline nlohmann::json jsonAt(const uint8_t* data, std::size_t size, std::size_t pos) {
    cbor::Reader r(data, size);
    r.seek(pos);
    r.skip();
    return nlohmann::json::from_cbor(data + pos, data + r.pos(), /*strict*/ true,
                                     /*allow_exceptions*/ false);
}

// ── The registry ──────────────────────────────────────────────────────────────
// The engine's components, in load order. Field-by-field where the saved form is
// the component's own data; a codec of its own where it is not (a mesh is saved
// as an asset reference and loaded through the asset system; a skeleton handle
// is session-local and never reaches disk).
//
// Each key is what scenes on disk already use, and must stay so: changing one is
// a format change, which is a revision with a migration (DR-0034), and
// tests/scene_schema_test.cpp holds the format byte for byte.
inline ComponentSchema customSchema(const char* name, const char* key, uint32_t revision,
                                    bool (*has)(flecs::entity),
                                    void (*save)(flecs::entity, nlohmann::json&, const SerdeContext&),
                                    void (*load)(flecs::entity, const nlohmann::json&, SerdeContext&),
                                    void (*remove)(flecs::entity)) {
    ComponentSchema s;
    s.name = name; s.key = key; s.revision = revision;
    s.has = has; s.save = save; s.load = load; s.remove = remove;
    return s;
}

inline void registerBuiltinSchemas(SchemaRegistry& r) {
    r.add(SchemaBuilder<Transform>("core.Transform", "transform")
        .vec3("position", &Transform::position)
        .quat("rotation", &Transform::rotation)
        .vec3("scale",    &Transform::scale)
        .build());
    // Name's saved form is a bare string, not an object: the one legacy shape kept.
    {
        ComponentSchema name = customSchema("core.Name", "name", Name::kAbiRevision, hasName,
                                            saveName, loadName,
                                            [](flecs::entity e) { e.remove<Name>(); });
        // A string read in place; anything else ("Entity") through loadName.
        name.loadCbor = [](flecs::entity e, const uint8_t* d, std::size_t n, std::size_t pos,
                           SerdeContext&, std::size_t* end) {
            const CborValue v(d, n, pos);
            if (!v.isString()) return false;
            e.set<Name>({v.asString()});
            if (end) *end = v.body;
            return true;
        };
        name.orderFree = true;
        // Packed: the string itself, as one field named "" (packed_scene.h).
        name.pack = [](const nlohmann::json& v, packed::Writer& w) {
            if (!v.is_string()) return false;   // "Entity", through loadName
            w.str(v.get_ref<const std::string&>());
            return true;
        };
        name.unpack = [](flecs::entity e, packed::Reader& r, SerdeContext&) {
            std::string value(r.str());
            if (!r.ok() || !r.atEnd()) return false;
            e.set<Name>({std::move(value)});
            return true;
        };
        name.packedFields = { {"", FieldType::String, 1} };
        r.add(std::move(name));
    }
    {
        ComponentSchema mesh = customSchema("core.MeshRenderer", "meshRenderer",
            MeshRenderer::kAbiRevision, hasMesh, saveMesh, loadMesh,
            [](flecs::entity e) { e.remove<MeshRenderer>(); e.remove<UnresolvedMesh>();
                                  e.remove<MeshPlaceholder>(); });
        // The cooked scene's reference read in place; its JSON text is built only
        // when the mesh does not resolve (to keep it, WO-029).
        mesh.loadCbor = [](flecs::entity e, const uint8_t* d, std::size_t n, std::size_t pos,
                           SerdeContext& ctx, std::size_t*) {
            if (ctx.mode == SerdeMode::Memory) return false;
            loadMeshFrom(e, CborValue(d, n, pos), [d, n, pos] { return jsonread::dumpLoaded(jsonAt(d, n, pos)); }, ctx);
            return true;
        };
        r.add(std::move(mesh));
    }
    // After meshRenderer, and it has to stay there: a chain describes the
    // coarser levels of THAT mesh, and load order is the registry's order.
    r.add(customSchema("core.LodMesh", "lodMesh", 0, hasLodMesh, saveLodMesh, loadLodMesh,
                       [](flecs::entity e) { e.remove<LodMesh>(); }));
    r.add(SchemaBuilder<Camera>("core.Camera", "camera")
        .boolean("isPrimary",            &Camera::isPrimary)
        .enumeration("projection",       &Camera::projection)
        .f32("fov",                      &Camera::fov)
        .f32("orthoSize",                &Camera::orthoSize)
        .f32("nearPlane",                &Camera::nearPlane)
        .f32("farPlane",                 &Camera::farPlane)
        .vec4("clearColor",         &Camera::clearColor)
        .build());
    // An unknown enum byte is kept AS READ: resolvedToneMapper() and
    // resolvedExposure() refuse it at use, so a scene saved by a newer engine
    // with a tone mapper this one lacks round-trips instead of being rewritten.
    r.add(SchemaBuilder<ColourGrading>("core.ColourGrading", "colourGrading")
        .u8("exposureMode",              &ColourGrading::exposureMode)
        .u8("toneMapper",                &ColourGrading::toneMapper)
        .f32("exposureEV",               &ColourGrading::exposureEV)
        .f32("aperture",                 &ColourGrading::aperture)
        .f32("shutterSeconds",           &ColourGrading::shutterSeconds)
        .f32("iso",                      &ColourGrading::iso)
        .string("lutPath",               &ColourGrading::lutPath, /*omitEmpty*/ true)
        .build());
    // A body saved without "bodyType" is KINEMATIC, as every reader of the
    // format has read it since the start (and docs/reference/scene-format.md
    // says), though RigidBody's own default is dynamic.
    RigidBody bodyDefaults;
    bodyDefaults.bodyType = PhysicsBodyType::Kinematic;
    r.add(SchemaBuilder<RigidBody>("core.RigidBody", "rigidBody")
        .defaults(bodyDefaults)
        .enumeration("bodyType",         &RigidBody::bodyType)
        .enumeration("shape",            &RigidBody::shape)
        .f32("mass",                     &RigidBody::mass)
        .f32("restitution",              &RigidBody::restitution)
        .f32("friction",                 &RigidBody::friction)
        .boolean("useGravity",           &RigidBody::useGravity)
        .vec3("halfExtent",         &RigidBody::halfExtent)
        .f32("radius",                   &RigidBody::radius)
        .f32("halfHeight",               &RigidBody::halfHeight)
        .build());
    // The path only: instanceId and started are the script backend's, at run time.
    r.add(SchemaBuilder<ScriptComponent>("core.Script", "script")
        .string("path",                  &ScriptComponent::scriptPath)
        .savedWhen([](const ScriptComponent& s) { return !s.scriptPath.empty(); })
        .build());
    // `grounded` and `footOffset` are run-time state, never saved.
    r.add(SchemaBuilder<CharacterController>("core.CharacterController", "characterController")
        .f32("radius",                   &CharacterController::radius)
        .f32("height",                   &CharacterController::height)
        .f32("maxSlopeDeg",              &CharacterController::maxSlopeDeg)
        .f32("stepHeight",               &CharacterController::stepHeight)
        .f32("mass",                     &CharacterController::mass)
        .f32("gravityScale",             &CharacterController::gravityScale)
        .build());
    // Parameters only: a light's world position and direction come from its
    // Transform at extraction.
    r.add(SchemaBuilder<Light>("core.Light", "light")
        .enumeration("type",             &Light::type)
        .vec3("color",              &Light::color)
        .f32("intensity",                &Light::intensity)
        .f32("range",                    &Light::range)
        .f32("spotInner",                &Light::spotInner)
        .f32("spotOuter",                &Light::spotOuter)
        .boolean("castShadows",          &Light::castShadows)
        .boolean("useTemperature",       &Light::useTemperature)
        .f32("temperatureK",             &Light::temperatureK)
        .build());
    r.add(customSchema("core.SkinnedMesh", "skinnedMesh", SkinnedMesh::kAbiRevision,
                       hasSkinnedMesh, saveSkinnedMesh, loadSkinnedMesh,
                       [](flecs::entity e) { e.remove<SkinnedMesh>(); }));
    r.add(customSchema("core.Animator", "animator", Animator::kAbiRevision,
                       hasAnimator, saveAnimator, loadAnimator,
                       [](flecs::entity e) { e.remove<Animator>(); }));
}

// The engine's registry. Built on first use, then only read; a provider adds its
// components at boot, before any scene loads.
inline SchemaRegistry& schemas() {
    static SchemaRegistry r = [] { SchemaRegistry s; registerBuiltinSchemas(s); return s; }();
    return r;
}
inline const SchemaRegistry& schemasOf(const SerdeContext& ctx) {
    return ctx.schemas ? *ctx.schemas : schemas();
}

// The keys of an entity's object that are not components.
inline bool isEntityField(const std::string& key) {
    return key == "id" || key == "parentId" || key == "reflected";
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
    for (const ComponentSchema& s : schemasOf(ctx).all()) {
        if (!s.has(e)) continue;
        nlohmann::json sub; s.save(e, sub, ctx);
        if (s.revision > 0 && sub.is_object()) sub[kRevisionKey] = s.revision;
        je[s.key] = std::move(sub);
    }
    // Components this engine could not read, exactly as they were loaded.
    if (const UnknownComponents* u = e.try_get<UnknownComponents>())
        for (const auto& [key, blob] : u->blobs)
            if (!je.contains(key))
                je[key] = nlohmann::json::parse(blob, nullptr, false);
    // Generic path: every meta-registered component the registry does not name
    // (kit components) + still-pending blobs.
    { nlohmann::json r; reflected::save(e, r); if (!r.empty()) je["reflected"] = std::move(r); }
    return je;
}

// ── Loading an entity, in stages both readers share ──────────────────────────
// ── Every read below is TYPE-TOLERANT, and it has to be ─────────────────────
// nlohmann's `value()` does NOT fall back to the default when the key exists
// with the wrong type — it THROWS `type_error`. A `.scene` is a text file that
// gets hand-edited, merged, and conflict-resolved, so wrong types are the normal
// failure, not the adversarial one. One `"fov": "60"` used to propagate out of
// scene load and take the editor or the player down with it (found by
// tests/fuzz_entity_serde_test.cpp, on roughly one case in twenty).
//
// The granularity is the point: a malformed COMPONENT is skipped and reported,
// and the rest of the entity still loads. Wrapping the whole function would turn
// one bad field into a missing entity; wrapping the whole load would turn it into
// a missing scene.
template <class Fn>
inline void tolerantLoad(const char* what, Fn&& fn) {
    try { fn(); }
    catch (const std::exception& ex) {
        LOG_WARN("Scene", "entity %s: malformed \"%s\" — skipped (%s)", "load", what, ex.what());
    }
}

// An anonymous entity (no flecs name path, so no duplicate-name aliasing) with
// its stable id: `wanted` when Preserve and free, else a fresh one.
inline flecs::entity beginEntity(flecs::world& w, uint64_t wanted, SerdeContext& ctx,
                                 IdPolicy policy, flecs::entity existing = flecs::entity()) {
    flecs::entity e = existing ? existing : w.entity();
    uint64_t id = policy == IdPolicy::Preserve ? wanted : 0;
    // Dedupe: never silently alias an id that is already live. Through the index
    // when the caller supplied one (O(1)); otherwise the O(n) scan, which is only
    // acceptable for one-off callers — see EntityIdIndex.
    if (ctx.idIndex) {
        const uint64_t fresh = ctx.idIndex->unique(id);
        if (id != 0 && fresh != id)
            LOG_WARN("Scene", "EntityId %llu already live — reassigning",
                     (unsigned long long)id);
        id = fresh;
        ctx.idIndex->add(id, e);
    } else if (id == 0 || findById(w, id).is_alive()) {
        if (id != 0) LOG_WARN("Scene", "EntityId %llu already live — reassigning",
                              (unsigned long long)id);
        do { id = generateEntityId(); } while (findById(w, id).is_alive());
    }
    e.set<EntityId>({id});
    return e;
}

// A component this engine cannot read, kept as loaded (DR-0034).
inline void keepUnknown(flecs::entity e, const std::string& key, const nlohmann::json& data) {
    if (!e.has<UnknownComponents>()) e.set<UnknownComponents>({});
    e.get_mut<UnknownComponents>().blobs[key] = jsonread::dumpLoaded(data);
}

// One registered component from its JSON value: migrated if it carries an older
// revision, kept if it cannot be read here, else loaded.
inline void loadComponentJson(flecs::entity e, const ComponentSchema& s,
                              const nlohmann::json& value, SerdeContext& ctx) {
    // Data at the current revision, which is nearly all of it, is read in
    // place; only data that needs a migration (or refusing) is copied.
    const bool marked = value.is_object() && value.contains(kRevisionKey);
    if (!marked && s.revision == 0) {
        tolerantLoad(s.key.c_str(), [&] { s.load(e, value, ctx); });
        return;
    }
    nlohmann::json data = value;
    std::string why;
    if (!migrateToCurrent(s, data, why)) {
        LOG_WARN("Scene", "%s (\"%s\") cannot be read here: %s — kept, and saved "
                 "back unchanged", s.name.c_str(), s.key.c_str(), why.c_str());
        keepUnknown(e, s.key, value);
        return;
    }
    tolerantLoad(s.key.c_str(), [&] { s.load(e, data, ctx); });
}

inline void loadUnknownJson(flecs::entity e, const std::string& key, const nlohmann::json& value) {
    LOG_WARN("Scene", "component \"%s\" is not registered here — kept, and saved back "
             "unchanged", key.c_str());
    keepUnknown(e, key, value);
}

// Generic path: apply reflected components that resolve now; stash the rest in
// ReflectedPending until their kit registers the type. Kit-authored components
// are the LEAST trustworthy shape in the file — the engine has never seen their
// schema — so this one is not optional.
inline void loadReflectedJson(flecs::entity e, const nlohmann::json& value) {
    tolerantLoad("reflected", [&] { reflected::load(e, value); });
}

// The skin of a mesh loaded above, now that the authored Animator is read.
inline void finishEntity(flecs::entity e, SerdeContext& ctx) {
    if (!ctx.skinPending) return;
    ctx.skinPending = false;
    if (const PendingSkin* ps = e.try_get<PendingSkin>()) {
        const PendingSkin skin = *ps;
        e.remove<PendingSkin>();
        wireSkin(e, skin.skeleton, skin.clips,
                 ctx.sceneAssets ? ctx.sceneAssets->bindClip : nullptr);
    }
}

// Creates an anonymous entity, resolves identity per policy, then loads every
// component: unknown keys kept, registered ones in registry order, a kit's last.
// Parent links are NOT restored here. Must be called outside query iteration.
inline flecs::entity createEntity(flecs::world& w, const nlohmann::json& je,
                                  SerdeContext& ctx, IdPolicy policy) {
    uint64_t wanted = 0;
    if (je.is_object())
        if (auto it = je.find("id"); it != je.end()) wanted = idFrom(JsonValue{&*it});
    flecs::entity e = beginEntity(w, wanted, ctx, policy);
    if (!je.is_object()) return e;
    // A scene loader passes its own list and resolves it after the last entity;
    // a lone entity (undo) resolves its references when its components are set.
    const bool ownRefs = beginEntityRefs(ctx);
    struct EndRefs { SerdeContext& c; flecs::world w; bool own;
                     ~EndRefs() { endEntityRefs(c, w, own); } } endRefs{ctx, w, ownRefs};
    const SchemaRegistry& reg = schemasOf(ctx);
    // Unknown first, so a component kept here is never also loaded below.
    for (auto it = je.begin(); it != je.end(); ++it)
        if (!isEntityField(it.key()) && !reg.byKey(it.key()))
            loadUnknownJson(e, it.key(), it.value());
    for (const ComponentSchema& s : reg.all())
        if (auto it = je.find(s.key); it != je.end()) loadComponentJson(e, s, *it, ctx);
    if (auto it = je.find("reflected"); it != je.end()) loadReflectedJson(e, *it);
    finishEntity(e, ctx);
    return e;
}

// ── The same, from the cooked scene's CBOR, with no JSON tree (WO-092) ───────
// `pos` is the entity's map in `data`, which cbor::validate has accepted. Each
// registered component with a CBOR reader is read in place; anything else (a
// codec of its own, an unknown key, a kit's block, a revision marker) is built
// as a small JSON value and goes through the same stage the JSON reader uses,
// so the two readers cannot disagree about it. Keys are taken as the tree takes
// them: the last of a repeated key wins.

struct CookedIds { uint64_t fileId = 0, parentId = 0; };

// One walk over an entity's map, loading it into `e`. Components that read
// nothing but their own value (`orderFree`) are loaded as their keys come; the
// rest wait and load in registry order, as the JSON reader loads everything.
// The id and parent keys are not loaded: their positions are handed back.
// Returns false, having loaded what it loaded, if the bytes are not what the
// cook writes (a failed read, or a key that repeats): the caller rolls back.
inline bool loadEntityCborMap(flecs::entity e, const uint8_t* data, std::size_t size,
                              std::size_t pos, SerdeContext& ctx,
                              std::size_t* idAtOut = nullptr, std::size_t* parentAtOut = nullptr) {
    const SchemaRegistry& reg = schemasOf(ctx);
    const auto& schemas = reg.all();
    static thread_local std::vector<std::size_t> later;
    static thread_local std::vector<uint8_t> seen;
    later.assign(schemas.size(), SIZE_MAX);
    seen.assign(schemas.size(), 0);
    std::size_t idAt = SIZE_MAX, parentAt = SIZE_MAX, reflectedAt = SIZE_MAX;
    std::vector<std::pair<std::string_view, std::size_t>> unknown;

    cbor::Reader r(data, size);
    r.seek(pos);
    cbor::Item obj;
    if (!r.head(obj) || obj.kind != cbor::Kind::Map) return false;
    for (uint64_t n = 0; n < obj.count; ++n) {
        cbor::Item key;
        if (!r.head(key) || key.kind != cbor::Kind::Text) return false;
        const std::size_t vpos = r.pos();
        auto once = [](std::size_t& slot, std::size_t v) {
            if (slot != SIZE_MAX) return false;
            slot = v;
            return true;
        };
        if (key.text == "id")             { if (!once(idAt, vpos)) return false; }
        else if (key.text == "parentId")  { if (!once(parentAt, vpos)) return false; }
        else if (key.text == "reflected") { if (!once(reflectedAt, vpos)) return false; }
        else {
            std::size_t k = 0;
            while (k < schemas.size() && key.text != schemas[k].key) ++k;
            if (k == schemas.size()) {
                for (const auto& u : unknown) if (u.first == key.text) return false;
                unknown.push_back({key.text, vpos});
            } else {
                if (seen[k]++) return false;
                const ComponentSchema& s = schemas[k];
                bool done = false;
                std::size_t end = SIZE_MAX;
                if (s.orderFree && s.loadCbor && s.revision == 0)
                    tolerantLoad(s.key.c_str(), [&] {
                        done = true;   // a throw is a load that reported and skipped
                        done = s.loadCbor(e, data, size, vpos, ctx, &end);
                    });
                if (!done) later[k] = vpos;
                else if (end != SIZE_MAX) { r.seek(end); continue; }
            }
        }
        r.seek(vpos);
        if (!r.skip()) return false;
    }
    if (r.failed()) return false;
    if (idAtOut) *idAtOut = idAt;
    if (parentAtOut) *parentAtOut = parentAt;

    // In the tree's order (sorted by key), as the JSON reader keeps them.
    std::sort(unknown.begin(), unknown.end());
    for (const auto& u : unknown)
        loadUnknownJson(e, std::string(u.first), jsonAt(data, size, u.second));
    for (std::size_t k = 0; k < schemas.size(); ++k) {
        if (later[k] == SIZE_MAX) continue;
        const ComponentSchema& s = schemas[k];
        bool done = false;
        if (s.loadCbor && s.revision == 0)
            tolerantLoad(s.key.c_str(), [&] { done = true; done = s.loadCbor(e, data, size, later[k], ctx, nullptr); });
        if (!done) loadComponentJson(e, s, jsonAt(data, size, later[k]), ctx);
    }
    if (reflectedAt != SIZE_MAX) loadReflectedJson(e, jsonAt(data, size, reflectedAt));
    return true;
}

// An entity from the cooked scene's CBOR. False if the bytes are not what the
// cook writes, having created what it created: the caller rolls the load back
// and reads the scene through the tree, which keeps nlohmann's rules exactly.
inline bool createEntityCbor(flecs::world& w, const uint8_t* data, std::size_t size,
                             std::size_t pos, SerdeContext& ctx, IdPolicy policy,
                             flecs::entity& out, CookedIds* ids = nullptr) {
    flecs::entity e = w.entity();   // its id is resolved after the walk
    out = e;
    const bool ownRefs = beginEntityRefs(ctx);
    struct EndRefs { SerdeContext& c; flecs::world w; bool own;
                     ~EndRefs() { endEntityRefs(c, w, own); } } endRefs{ctx, w, ownRefs};
    std::size_t idAt = SIZE_MAX, parentAt = SIZE_MAX;
    if (!loadEntityCborMap(e, data, size, pos, ctx, &idAt, &parentAt)) return false;
    if (ids) {
        ids->fileId   = idAt == SIZE_MAX ? 0 : u64From(CborValue(data, size, idAt));
        ids->parentId = parentAt == SIZE_MAX ? 0 : u64From(CborValue(data, size, parentAt));
    }
    beginEntity(w, idAt == SIZE_MAX ? 0 : idFrom(CborValue(data, size, idAt)), ctx, policy, e);
    finishEntity(e, ctx);
    return true;
}

// ── The packed scene (WO-092, assets/packed_scene.h) ────────────────────────
inline SerdeContext& packContext() {
    static thread_local SerdeContext ctx;
    static thread_local std::vector<SerdeContext::EntityRef> sink;
    ctx.mode = SerdeMode::Disk;
    sink.clear();
    ctx.entityRefs = &sink;
    return ctx;
}

// A packed record rebuilt as the JSON value it was packed from, by the cooked
// file's own field table: for a component whose schema here is not the one it
// was cooked with (another revision, other fields) or is not registered at all.
// With its cooked revision marked, so the JSON reader migrates it.
inline bool packedToJson(const packed::Schema& cooked, const uint8_t* data, uint32_t size,
                         nlohmann::json& out) {
    packed::Reader r(data, size);
    auto value = [&](const packed::Field& f) -> nlohmann::json {
        const FieldType t = (FieldType)f.type;
        auto floats = [&](std::size_t n) {
            nlohmann::json a = nlohmann::json::array();
            for (std::size_t k = 0; k < n; ++k) { float v = 0; r.raw(&v, 4); a.push_back(v); }
            return a;
        };
        switch (t) {
            case FieldType::Bool: return r.u8() != 0;
            case FieldType::I8:  { int8_t v = 0;   r.raw(&v, 1); return v; }
            case FieldType::U8:  { uint8_t v = 0;  r.raw(&v, 1); return v; }
            case FieldType::I16: { int16_t v = 0;  r.raw(&v, 2); return v; }
            case FieldType::U16: { uint16_t v = 0; r.raw(&v, 2); return v; }
            case FieldType::I32: { int32_t v = 0;  r.raw(&v, 4); return v; }
            case FieldType::U32: { uint32_t v = 0; r.raw(&v, 4); return v; }
            case FieldType::I64: { int64_t v = 0;  r.raw(&v, 8); return v; }
            case FieldType::U64: { uint64_t v = 0; r.raw(&v, 8); return v; }
            case FieldType::F32:
                if (f.count > 1) return floats(f.count);
                { float v = 0; r.raw(&v, 4); return v; }
            case FieldType::F64: { double v = 0; r.raw(&v, 8); return v; }
            case FieldType::Vec2: return floats(2);
            case FieldType::Vec3: return floats(3);
            case FieldType::Vec4: case FieldType::Quat: return floats(4);
            case FieldType::Mat4: return floats(16);
            case FieldType::String: return std::string(r.str());
            case FieldType::Entity: {
                const bool present = r.u8() != 0;
                const uint64_t id = r.u64();
                return present ? nlohmann::json(id) : nlohmann::json();   // null: absent
            }
            case FieldType::Asset: {
                AssetRef a; a.uuid = std::string(r.str()); a.path = std::string(r.str());
                nlohmann::json o = nlohmann::json::object();
                assetref::toJson(a, o);
                return o;
            }
        }
        r.fail();
        return nullptr;
    };
    if (cooked.fields.size() == 1 && cooked.fields[0].name.empty()) {
        out = value(cooked.fields[0]);          // the value itself (Name's string)
    } else {
        out = nlohmann::json::object();
        for (const packed::Field& f : cooked.fields) {
            nlohmann::json v = value(f);
            if (f.type == (uint8_t)FieldType::Entity && v.is_null()) continue;
            out[f.name] = std::move(v);
        }
        if (cooked.revision > 0) out[kRevisionKey] = cooked.revision;
    }
    return r.ok() && r.atEnd();
}

// Writes `scene` (the .scene's JSON, mesh references already healed by the
// cooker) as a packed payload. Each registered component with a packed form,
// and data that reads at this engine's revision, is packed; everything else
// (codecs of their own, unknown keys, a kit's block, data no migration reaches)
// goes in the entity's rest, as CBOR, unchanged. Deterministic: the schema table
// in registry order, keys in the tree's sorted order.
inline bool packScene(const nlohmann::json& scene, assetlib::SceneAsset& out, std::string& why,
                      const SchemaRegistry& reg = schemas()) {
    out.header = assetlib::SceneHeader{};
    out.header.encoding = (uint32_t)assetlib::ScenePayload::Packed;
    out.payload.clear();
    packed::Writer w(out.payload);
    w.u32(packed::kMagic);
    w.u32(packed::kLayout);

    std::vector<const ComponentSchema*> table;
    for (const ComponentSchema& s : reg.all())
        if (s.pack && s.unpack && !s.packedFields.empty()) table.push_back(&s);
    w.u32((uint32_t)table.size());
    for (const ComponentSchema* s : table) {
        w.str(s->name); w.str(s->key); w.u32(s->revision);
        w.u32((uint32_t)s->packedFields.size());
        for (const FieldInfo& f : s->packedFields) { w.str(f.name); w.u8((uint8_t)f.type); w.u32(f.count); }
    }
    auto indexOf = [&](const ComponentSchema* s) -> int {
        for (std::size_t k = 0; k < table.size(); ++k) if (table[k] == s) return (int)k;
        return -1;
    };

    const std::size_t countAt = w.reserve32();
    uint32_t count = 0;
    if (scene.is_object() && scene.contains("entities") && scene["entities"].is_array()) {
        for (const nlohmann::json& je : scene["entities"]) {
            if (!je.is_object()) continue;   // the readers skip it; so does the pack
            ++count;
            uint64_t wanted = 0, fileId = 0, parentId = 0;
            if (auto it = je.find("id"); it != je.end()) {
                wanted = idFrom(JsonValue{&*it});
                fileId = u64From(JsonValue{&*it});
            }
            if (auto it = je.find("parentId"); it != je.end()) parentId = u64From(JsonValue{&*it});
            w.u64(wanted); w.u64(fileId); w.u64(parentId);

            nlohmann::json rest = nlohmann::json::object();
            const std::size_t recordsAt = w.reserve32();
            uint32_t records = 0;
            for (auto it = je.begin(); it != je.end(); ++it) {
                if (it.key() == "id" || it.key() == "parentId") continue;
                const ComponentSchema* s = reg.byKey(it.key());
                const int idx = s ? indexOf(s) : -1;
                bool packedIt = false;
                if (idx >= 0) {
                    // At this revision, as the JSON reader would load it.
                    const nlohmann::json* value = &*it;
                    nlohmann::json migrated;
                    const bool marked = it->is_object() && it->contains(kRevisionKey);
                    bool readable = true;
                    if (marked || s->revision > 0) {
                        migrated = *it;
                        std::string ignore;
                        readable = it->is_object() && migrateToCurrent(*s, migrated, ignore);
                        value = &migrated;
                    }
                    if (readable) {
                        std::vector<uint8_t> bytes;
                        packed::Writer rw(bytes);
                        if (s->pack(*value, rw)) {
                            w.u32((uint32_t)idx);
                            w.u32((uint32_t)bytes.size());
                            w.raw(bytes.data(), bytes.size());
                            ++records;
                            packedIt = true;
                        }
                    }
                }
                if (!packedIt) rest[it.key()] = *it;
            }
            w.patch32(recordsAt, records);
            if (rest.empty()) {
                w.u32(0);
            } else {
                const std::vector<uint8_t> cbor = nlohmann::json::to_cbor(rest);
                w.u32((uint32_t)cbor.size());
                w.raw(cbor.data(), cbor.size());
            }
        }
    }
    w.patch32(countAt, count);
    (void)why;
    return true;
}

// The packer a cook installs (cooked_scene::Packer), over the engine's schemas.
inline cooked_scene::Packer scenePacker() {
    return [](const nlohmann::json& scene, assetlib::SceneAsset& out, std::string& why) {
        return packScene(scene, out, why);
    };
}

// The cooked file's schemas, matched to this engine's: a record whose schema
// here is the one it was cooked with (name, key, revision, packed fields) is
// unpacked; any other is rebuilt as JSON (packedToJson) and read through the
// JSON stage, or kept as unknown when nothing here has its name.
struct PackedSchemas {
    std::vector<const ComponentSchema*> same;    // by cooked index; null: not the same
    std::vector<const ComponentSchema*> byName;  // by cooked index; null: not registered
};
inline PackedSchemas matchPackedSchemas(const packed::View& view, const SchemaRegistry& reg) {
    PackedSchemas m;
    for (const packed::Schema& c : view.schemas()) {
        const ComponentSchema* s = reg.byName(c.name);
        bool same = s && s->unpack && s->key == c.key && s->revision == c.revision
                 && s->packedFields.size() == c.fields.size();
        for (std::size_t k = 0; same && k < c.fields.size(); ++k) {
            const FieldInfo& f = s->packedFields[k];
            same = f.name == c.fields[k].name && (uint8_t)f.type == c.fields[k].type
                && f.count == c.fields[k].count;
        }
        m.same.push_back(same ? s : nullptr);
        m.byName.push_back(s);
    }
    return m;
}

// An entity from a packed scene. False, having created what it created, on
// bytes that do not fit: the caller rolls the load back and refuses the file.
inline bool createEntityPacked(flecs::world& w, const packed::View& view, const packed::Entity& pe,
                               const PackedSchemas& match, SerdeContext& ctx, IdPolicy policy,
                               flecs::entity& out) {
    flecs::entity e = w.entity();
    out = e;
    const bool ownRefs = beginEntityRefs(ctx);
    struct EndRefs { SerdeContext& c; flecs::world w; bool own;
                     ~EndRefs() { endEntityRefs(c, w, own); } } endRefs{ctx, w, ownRefs};
    for (const packed::Record& rec : pe.records) {
        if (const ComponentSchema* s = match.same[rec.schema]) {
            packed::Reader r(rec.data, rec.size);
            if (!s->unpack(e, r, ctx)) return false;
            continue;
        }
        const packed::Schema& cooked = view.schemas()[rec.schema];
        nlohmann::json value;
        if (!packedToJson(cooked, rec.data, rec.size, value)) return false;
        if (const ComponentSchema* s = match.byName[rec.schema]) loadComponentJson(e, *s, value, ctx);
        else loadUnknownJson(e, cooked.key, value);
    }
    if (pe.rest && !loadEntityCborMap(e, pe.rest, pe.restSize, 0, ctx)) return false;
    beginEntity(w, pe.wantedId, ctx, policy, e);
    finishEntity(e, ctx);
    return true;
}

} // namespace EntitySerde
