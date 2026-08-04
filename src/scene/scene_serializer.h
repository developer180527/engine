#pragma once
#include "core/transform_utils.h"
#include "scene/entity_serializer.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <flecs.h>

#include "core/transform.h"
#include "components/name.h"
#include "components/spinner.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/mesh_renderer.h"
#include "components/entity_id.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "assets/asset_storage.h"
#include "assets/importers/importer_registry.h"
#include "runtime/services/async_loader.h"
#include "runtime/services/asset_service.h"
#include "animation/clip_library.h"
#include "render/primitive_library.h"
#include "core/entity_id_util.h"
#include "core/logger.h"
#include <assetlib/asset_registry.h>
#include <assetlib/scene_asset.h>
#include "assets/cookers/scene/scene_cooker.h"

// SceneSerializer is now a thin orchestration layer over EntitySerde: it owns
// which entities are persisted and the async asset plumbing, while all
// component (de)serialization goes through the one shared table.
namespace SceneSerializer {

using EntitySerde::SerdeContext;
using EntitySerde::SerdeMode;
using EntitySerde::IdPolicy;
using EntitySerde::PendingMesh;

// Restore ChildOf links by stable id, after all entities exist. Cycle-guarded.
inline void restoreParents(flecs::world& w, const nlohmann::json& scene) {
    std::unordered_map<uint64_t, flecs::entity> byId;
    w.query_builder<const EntityId>().build()
        .each([&](flecs::entity e, const EntityId& id) { byId[id.value] = e; });
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        uint64_t cid = je.value("id", (uint64_t)0);
        uint64_t pid = je.value("parentId", (uint64_t)0);
        if (!cid || !pid) continue;
        auto ci = byId.find(cid);
        auto pi = byId.find(pid);
        if (ci == byId.end() || pi == byId.end()) continue;
        // safeReparent guards cycles AND over-deep chains (flecs aborts past
        // FLECS_DAG_DEPTH_MAX) — a malformed scene can't crash the engine.
        safeReparent(ci->second, pi->second);
    }
}

// ── Disk save ────────────────────────────────────────────────────────────────
inline bool save(const std::filesystem::path& path,
                 flecs::world& ecs, AssetRegistry& assets,
                 assetlib::AssetRegistry* assetLib = nullptr,
                 const std::filesystem::path& projectRoot = {}) {
    std::filesystem::create_directories(path.parent_path());
    assignMissingIds(ecs);                              // before the query: set<> is illegal mid-iteration

    SerdeContext ctx;
    ctx.mode        = SerdeMode::Disk;
    ctx.meshLookup  = [&assets](MeshHandle h) -> const Mesh* { return assets.getMesh(h); };
    ctx.projectRoot = projectRoot;
    ctx.assetLib    = assetLib;

    // When the assetlib DB is available, resolve source paths → cooked paths
    // so runtime loading can skip Assimp and load straight from cooked binaries.
    if (assetLib && !projectRoot.empty()) {
        ctx.cookedPathLookup = [assetLib, projectRoot](const std::string& sourcePath)
                -> std::string {
            // Primitives don't have cooked counterparts
            if (sourcePath.rfind("engine://", 0) == 0) return {};
            // Compute project-relative path for the DB lookup
            std::error_code ec;
            auto rel = std::filesystem::relative(sourcePath, projectRoot, ec);
            if (ec || rel.empty()) return {};
            auto rec = assetLib->findBySourcePath(rel.generic_string());
            if (rec && rec->state == assetlib::AssetState::Ready
                    && !rec->cookedPath.empty())
                return rec->cookedPath;
            return {};
        };
    }

    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();
    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name&, const Transform&) {
            if (e.has<Spinner>()) return;               // procedural — not persisted
            scene["entities"].push_back(EntitySerde::saveEntity(e, ctx));
        });

    std::ofstream f(path);
    f << scene.dump(2);
    LOG_SUCCESS("Scene", "Saved %zu entities → %s",
                scene["entities"].size(), path.filename().string().c_str());
    return f.good();
}

// ── Disk load (async asset streaming) ────────────────────────────────────────
// When assetService is provided, entities with a cookedPath in the scene file
// load directly from cooked binaries (fast runtime path). Entities without a
// cookedPath fall through to the legacy import (glTF / Assimp).
inline bool loadAsync(const std::filesystem::path& scenePath,
                      flecs::world&     ecs,
                      AssetStorage&     storage,
                      AsyncLoader&      loader,
                      ImporterRegistry& importers,
                      PrimitiveLibrary* primitives = nullptr,
                      AssetService*     assetService = nullptr,
                      const std::filesystem::path& projectRoot = {},
                      assetlib::AssetRegistry*     assetLib = nullptr,
                      ClipLibrary*                 clipLib = nullptr) {
    if (!std::filesystem::exists(scenePath)) {
        LOG_WARN("Scene", "Not found: %s", scenePath.string().c_str());
        return false;
    }
    nlohmann::json scene;
    try {
        std::ifstream f(scenePath);
        scene = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        LOG_ERROR("Scene", "Parse error: %s", e.what());
        return false;
    }

    std::vector<PendingMesh> pending;
    SerdeContext ctx;
    ctx.mode         = SerdeMode::Disk;
    ctx.assetService = assetService;
    ctx.importers    = &importers;
    ctx.storage      = &storage;
    ctx.primitives   = primitives;
    ctx.pendingAsync = &pending;
    ctx.projectRoot  = projectRoot;
    ctx.assetLib     = assetLib;

    // ONE query for the id index, then O(1) collision checks. Without this,
    // createEntity falls back to findById per entity and the load is quadratic.
    EntityIdIndex ids;
    ids.build(ecs);
    ctx.idIndex = &ids;

    for (const auto& je : scene.value("entities", nlohmann::json::array()))
        EntitySerde::createEntity(ecs, je, ctx, IdPolicy::Preserve);

    // Stream deferred (Assimp) meshes on worker threads; wire the handle in
    // on completion. Immediate cases (primitive, glTF, cooked) were resolved
    // in-load — only truly unresolved meshes end up here.
    for (auto& pm : pending) {
        const Name* n = pm.entity.try_get<Name>();
        std::string label = n ? n->value : std::string{};
        flecs::world*     pw       = &ecs;
        flecs::entity_t   eid      = pm.entity.id();
        SkeletonRegistry* skels    = storage.skeletons;   // runtime-owned, outlive the load
        AnimClipRegistry* clipsReg = storage.clips;
        loader.load(pm.assetPath, label,
            [pw, eid, skels, clipsReg, clipLib](const AsyncLoadResult& r, const std::string&) {
                if (!r.mesh.valid()) return;
                flecs::entity e = pw->entity(eid);
                if (e.id() == 0 || !e.is_alive()) return;
                e.set<MeshRenderer>({r.mesh});
                // Restore skeletal animation if the asset has bones.
                // Handles are session-local — the scene file stores identity
                // only (Animator::clipPath asset ref, or legacy clipIndex);
                // handles are resolved HERE, from this import, never from disk.
                if (r.skeleton.valid()) {
                    SkinnedMesh sm;
                    sm.skeleton = r.skeleton;
                    e.set<SkinnedMesh>(sm);

                    Animator anim;
                    bool hadAnimator = false;
                    if (const Animator* a = e.try_get<Animator>()) {
                        anim = *a;
                        hadAnimator = true;
                    }
                    if (!anim.clipPath.empty() && clipLib && skels && clipsReg) {
                        // Standalone clip asset: bind by bone name to THIS
                        // entity's freshly imported skeleton.
                        if (const Skeleton* sk = skels->get(r.skeleton)) {
                            AnimClipHandle h = clipLib->load(anim.clipPath,
                                                             r.skeleton, *sk, *clipsReg);
                            if (h.valid()) {
                                anim.clip = h;
                                if (!hadAnimator) anim.playing = true;
                            }
                        }
                    } else if (!r.clips.empty()) {   // legacy: clip inside the mesh file
                        int idx = anim.clipIndex;
                        if (idx < 0 || idx >= (int)r.clips.size()) idx = 0;
                        anim.clipIndex = idx;
                        anim.clip      = r.clips[idx];
                        if (!hadAnimator) anim.playing = true; // sensible default
                    }
                    e.set<Animator>(anim);
                }
            });
    }

    restoreParents(ecs, scene);
    LOG_INFO("Scene", "Loaded scene → %s (%zu async)",
             scenePath.filename().string().c_str(), pending.size());
    return true;
}

// ── In-memory snapshot (editor world -> JSON string) ─────────────────────────
inline std::string saveToString(flecs::world& ecs, AssetStorage& assets) {
    assignMissingIds(ecs);
    SerdeContext ctx;
    ctx.mode       = SerdeMode::Memory;
    ctx.meshLookup = [&assets](MeshHandle h) -> const Mesh* { return assets.meshes.getMesh(h); };

    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();
    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name&, const Transform&) {
            if (e.has<Spinner>()) return;
            scene["entities"].push_back(EntitySerde::saveEntity(e, ctx));
        });
    return scene.dump();
}

// ── Instant game-world population from snapshot (live handle reuse) ──────────
inline void loadIntoWorld(const std::string& snapshot, flecs::world& world,
                          AssetStorage& /*assets*/) {
    nlohmann::json scene;
    try { scene = nlohmann::json::parse(snapshot); }
    catch (const std::exception& ex) {
        LOG_ERROR("Scene", "loadIntoWorld parse error: %s", ex.what());
        return;
    }
    SerdeContext ctx;
    ctx.mode = SerdeMode::Memory;                       // reuse live handle ids directly

    EntityIdIndex ids;                                  // see the disk path
    ids.build(world);
    ctx.idIndex = &ids;

    int count = 0;
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        EntitySerde::createEntity(world, je, ctx, IdPolicy::Preserve);
        ++count;
    }
    restoreParents(world, scene);
    LOG_INFO("Scene", "Game world populated: %d entities (instant, shared handles)", count);
}

// ── Cook: JSON scene → binary scene ─────────────────────────────────────────
// Delegates to cookSceneFile() in cookers/scene_cooker.cpp. This thin wrapper
// keeps the same call-site API; the real logic lives in a standalone translation
// unit with minimal includes (so CookService can use it without pulling in flecs,
// entity_serializer, etc.).
inline bool cookScene(const std::filesystem::path& jsonPath,
                      const std::filesystem::path& outPath,
                      assetlib::AssetRegistry* assetLib = nullptr,
                      const std::filesystem::path& projectRoot = {}) {
    return cookSceneFile(jsonPath, outPath, assetLib, projectRoot);
}

} // namespace SceneSerializer
