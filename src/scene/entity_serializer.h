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
};

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
inline void loadMeshRef(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    if (ctx.mode == SerdeMode::Memory) {
        if (auto it = j.find("unresolved"); it != j.end() && it->is_object()) {
            e.set<UnresolvedMesh>({it->dump(), j.value("unresolvedReason", std::string{}), false});
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
    // path below that sets a MeshRenderer removes this; every path that does not
    // leaves it, with the reason, so the next save cannot drop the mesh.
    e.set<UnresolvedMesh>({j.dump(), "not resolved", false});
    auto unresolved = [&e](std::string why, bool pending = false) {
        if (UnresolvedMesh* u = e.try_get_mut<UnresolvedMesh>()) {
            u->reason = std::move(why); u->pending = pending;
        }
    };
    auto resolved = [&e](MeshRenderer mr) {
        e.set<MeshRenderer>(mr);
        e.remove<UnresolvedMesh>();
        e.remove<MeshPlaceholder>();
    };

    // The authored material name, resolved to a handle. Empty is normal — most
    // entities use the material baked into their mesh.
    MaterialHandle matOverride;
    const SceneAssets* sa = ctx.sceneAssets;
    if (sa && sa->loadMaterial) {
        const std::string matName = j.value("material", std::string{});
        if (!matName.empty()) {
            matOverride = sa->loadMaterial(matName.c_str());
            if (!matOverride.valid())
                LOG_WARN("Scene", "material \"%s\" not found — the mesh's own "
                         "material is used instead", matName.c_str());
        }
    }

    const std::string cookedPath = j.value("cookedPath", std::string{});
    if (!cookedPath.empty() && sa && sa->loadCookedMesh) {
        // Request the cooked LOD chain too. Without this the editor's load path
        // silently produced no levels while scene_service's did — the same
        // asset rendering with LOD in the player and without it in the editor.
        std::vector<MeshHandle> lods;
        MeshHandle h = sa->loadCookedMesh(cookedPath.c_str(), &lods);
        if (h.valid()) {
            resolved({h, matOverride});
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
    AssetRef    ref     = assetref::fromJson(j);
    std::string srcPath = assetref::resolve(ref, ctx.projectRoot, ctx.assetLib);
    std::string srcType = j.value("sourceType", std::string{});
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
inline void loadMesh(flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
    loadMeshRef(e, j, ctx);
    if (ctx.mode != SerdeMode::Disk || !e.has<UnresolvedMesh>() || e.has<MeshRenderer>()) return;
    if (!ctx.primitives || !ctx.primitives->ready()) return;
    const MeshHandle cube = ctx.primitives->cube();
    e.set<MeshRenderer>({cube});
    e.set<MeshPlaceholder>({cube});
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
                h = ctx.sceneAssets->loadCookedMesh(cooked.c_str(), nullptr);   // relative — see above
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
        AssetRef ref = assetref::fromJson(j);
        if (!ref.empty())
            a.clipPath = assetref::resolve(ref, ctx.projectRoot, ctx.assetLib);
    }
    a.clipIndex = j.value("clipIndex", 0);
    a.fade    = readFloat(j, "fade", 0.2f);
    a.time    = readFloat(j, "time", 0.0f);
    a.speed   = readFloat(j, "speed", 1.0f);
    a.playing = j.value("playing", false);
    a.looping = j.value("looping", true);
    e.set<Animator>(a);
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
        .floats<3>("position", &Transform::position)
        .floats<4>("rotation", &Transform::rotation)
        .floats<3>("scale",    &Transform::scale)
        .build());
    // Name's saved form is a bare string, not an object: the one legacy shape kept.
    r.add(customSchema("core.Name", "name", Name::kAbiRevision, hasName, saveName, loadName,
                       [](flecs::entity e) { e.remove<Name>(); }));
    r.add(customSchema("core.MeshRenderer", "meshRenderer", MeshRenderer::kAbiRevision,
                       hasMesh, saveMesh, loadMesh,
                       [](flecs::entity e) { e.remove<MeshRenderer>(); e.remove<UnresolvedMesh>();
                                             e.remove<MeshPlaceholder>(); }));
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
        .floats<4>("clearColor",         &Camera::clearColor)
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
    r.add(SchemaBuilder<RigidBody>("core.RigidBody", "rigidBody")
        .enumeration("bodyType",         &RigidBody::bodyType)
        .enumeration("shape",            &RigidBody::shape)
        .f32("mass",                     &RigidBody::mass)
        .f32("restitution",              &RigidBody::restitution)
        .f32("friction",                 &RigidBody::friction)
        .boolean("useGravity",           &RigidBody::useGravity)
        .floats<3>("halfExtent",         &RigidBody::halfExtent)
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
        .floats<3>("color",              &Light::color)
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

// Creates an anonymous entity (no flecs name path -> no duplicate-name
// aliasing), resolves identity per policy, then drives every table load.
// Parent links are NOT restored here. Must be called outside query iteration.
inline flecs::entity createEntity(flecs::world& w, const nlohmann::json& je,
                                  SerdeContext& ctx, IdPolicy policy) {
    flecs::entity e = w.entity();                       // anonymous
    // ── Every read below is TYPE-TOLERANT, and it has to be ─────────────────
    // nlohmann's `value()` does NOT fall back to the default when the key
    // exists with the wrong type — it THROWS `type_error`. A `.scene` is a text
    // file that gets hand-edited, merged, and conflict-resolved, so wrong types
    // are the normal failure, not the adversarial one. `scene_serializer.h`
    // wraps only `json::parse` in try/catch, which means one `"fov": "60"`
    // propagated an exception out of scene load and took the editor or the
    // player down with it. Found by tests/fuzz_entity_serde_test.cpp, which
    // reported it on roughly one case in twenty.
    //
    // The granularity is the point: a malformed COMPONENT is skipped and
    // reported, and the rest of the entity still loads. Wrapping the whole
    // function would turn one bad field into a missing entity; wrapping the
    // whole load — the obvious fix — would turn it into a missing scene.
    auto tolerant = [&](const char* what, auto&& fn) {
        try { fn(); }
        catch (const std::exception& ex) {
            LOG_WARN("Scene", "entity %s: malformed \"%s\" — skipped (%s)",
                     "load", what, ex.what());
        }
    };

    uint64_t id = 0;
    if (policy == IdPolicy::Preserve)
        tolerant("id", [&] { id = je.value("id", (uint64_t)0); });
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
    if (!je.is_object()) return e;
    const SchemaRegistry& reg = schemasOf(ctx);
    // Unknown first, so a component kept here is never also loaded below.
    auto keep = [&e](const std::string& key, const nlohmann::json& data) {
        if (!e.has<UnknownComponents>()) e.set<UnknownComponents>({});
        e.get_mut<UnknownComponents>().blobs[key] = data.dump();
    };
    for (auto it = je.begin(); it != je.end(); ++it)
        if (!isEntityField(it.key()) && !reg.byKey(it.key())) {
            LOG_WARN("Scene", "component \"%s\" is not registered here — kept, and "
                     "saved back unchanged", it.key().c_str());
            keep(it.key(), it.value());
        }
    for (const ComponentSchema& s : reg.all()) {
        auto it = je.find(s.key);
        if (it == je.end()) continue;
        nlohmann::json data = *it;
        std::string why;
        if (!migrateToCurrent(s, data, why)) {
            LOG_WARN("Scene", "%s (\"%s\") cannot be read here: %s — kept, and saved "
                     "back unchanged", s.name.c_str(), s.key.c_str(), why.c_str());
            keep(s.key, *it);
            continue;
        }
        tolerant(s.key.c_str(), [&] { s.load(e, data, ctx); });
    }
    // Generic path: apply reflected components that resolve now; stash the
    // rest in ReflectedPending until their kit registers the type. Kit-authored
    // components are the LEAST trustworthy shape in the file — the engine has
    // never seen their schema — so this one is not optional.
    if (je.contains("reflected"))
        tolerant("reflected", [&] { reflected::load(e, je["reflected"]); });
    return e;
}

} // namespace EntitySerde
