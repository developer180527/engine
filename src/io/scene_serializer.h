#pragma once
#include "core/transform_utils.h"
#include "io/entity_serializer.h"
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
#include "io/asset_storage.h"
#include "io/importer_registry.h"
#include "engine/async_loader.h"
#include "render/primitive_library.h"
#include "core/entity_id_util.h"
#include "engine/logger.h"

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
        flecs::entity child  = ci->second;
        flecs::entity parent = pi->second;
        if (child.is_alive() && parent.is_alive() && child != parent
            && !isAncestorOf(child, parent))
            child.add(flecs::ChildOf, parent);
    }
}

// ── Disk save ────────────────────────────────────────────────────────────────
inline bool save(const std::filesystem::path& path,
                 flecs::world& ecs, AssetRegistry& assets) {
    std::filesystem::create_directories(path.parent_path());
    assignMissingIds(ecs);                              // before the query: set<> is illegal mid-iteration

    SerdeContext ctx;
    ctx.mode       = SerdeMode::Disk;
    ctx.meshLookup = [&assets](MeshHandle h) -> const Mesh* { return assets.getMesh(h); };

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
inline bool loadAsync(const std::filesystem::path& scenePath,
                      flecs::world&     ecs,
                      AssetStorage&     storage,
                      AsyncLoader&      loader,
                      ImporterRegistry& importers,
                      PrimitiveLibrary* primitives = nullptr) {
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
    ctx.importers    = &importers;
    ctx.storage      = &storage;
    ctx.primitives   = primitives;
    ctx.pendingAsync = &pending;

    for (const auto& je : scene.value("entities", nlohmann::json::array()))
        EntitySerde::createEntity(ecs, je, ctx, IdPolicy::Preserve);

    // Stream deferred (Assimp) meshes on worker threads; wire the handle in
    // on completion. Immediate cases (primitive, glTF) were resolved in-load.
    for (auto& pm : pending) {
        const Name* n = pm.entity.try_get<Name>();
        std::string label = n ? n->value : std::string{};
        flecs::world*   pw  = &ecs;
        flecs::entity_t eid = pm.entity.id();
        loader.load(pm.assetPath, label,
            [pw, eid](MeshHandle h, const std::string&) {
                if (!h.valid()) return;
                flecs::entity e = pw->entity(eid);
                if (e.id() != 0 && e.is_alive())
                    e.set<MeshRenderer>({h});
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

    int count = 0;
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        EntitySerde::createEntity(world, je, ctx, IdPolicy::Preserve);
        ++count;
    }
    restoreParents(world, scene);
    LOG_INFO("Scene", "Game world populated: %d entities (instant, shared handles)", count);
}

} // namespace SceneSerializer
