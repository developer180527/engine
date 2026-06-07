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
#include "engine/asset_service.h"
#include "render/primitive_library.h"
#include "core/entity_id_util.h"
#include "engine/logger.h"
#include <assetlib/asset_registry.h>
#include <assetlib/scene_asset.h>

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
                 flecs::world& ecs, AssetRegistry& assets,
                 assetlib::AssetRegistry* assetLib = nullptr,
                 const std::filesystem::path& projectRoot = {}) {
    std::filesystem::create_directories(path.parent_path());
    assignMissingIds(ecs);                              // before the query: set<> is illegal mid-iteration

    SerdeContext ctx;
    ctx.mode       = SerdeMode::Disk;
    ctx.meshLookup = [&assets](MeshHandle h) -> const Mesh* { return assets.getMesh(h); };

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
                      AssetService*     assetService = nullptr) {
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

    for (const auto& je : scene.value("entities", nlohmann::json::array()))
        EntitySerde::createEntity(ecs, je, ctx, IdPolicy::Preserve);

    // Stream deferred (Assimp) meshes on worker threads; wire the handle in
    // on completion. Immediate cases (primitive, glTF, cooked) were resolved
    // in-load — only truly unresolved meshes end up here.
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

// ── Cook: JSON scene → binary scene ─────────────────────────────────────────
// Reads a JSON .scene file and writes a binary .scene.cooked. The binary
// format has zero parsing overhead at runtime — memcpy entities + string table.
// assetLib + projectRoot are used to resolve source paths → cooked paths.
inline bool cookScene(const std::filesystem::path& jsonPath,
                      const std::filesystem::path& outPath,
                      assetlib::AssetRegistry* assetLib = nullptr,
                      const std::filesystem::path& projectRoot = {}) {
    if (!std::filesystem::exists(jsonPath)) {
        LOG_ERROR("SceneCook", "JSON scene not found: %s", jsonPath.string().c_str());
        return false;
    }
    nlohmann::json scene;
    try {
        std::ifstream f(jsonPath);
        scene = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        LOG_ERROR("SceneCook", "Parse error: %s", e.what());
        return false;
    }

    assetlib::SceneAsset cooked;
    const auto entities = scene.value("entities", nlohmann::json::array());

    for (const auto& je : entities) {
        assetlib::SceneEntity ent{};
        ent.entityId = je.value("id", (uint64_t)0);
        ent.parentId = je.value("parentId", (uint64_t)0);

        // Transform
        if (je.contains("transform")) {
            ent.componentMask |= assetlib::kComp_Transform;
            const auto& t = je["transform"];
            if (t.contains("position")) {
                ent.position[0] = t["position"][0];
                ent.position[1] = t["position"][1];
                ent.position[2] = t["position"][2];
            }
            if (t.contains("rotation")) {
                ent.rotation[0] = t["rotation"][0];
                ent.rotation[1] = t["rotation"][1];
                ent.rotation[2] = t["rotation"][2];
                ent.rotation[3] = t["rotation"][3];
            }
            if (t.contains("scale")) {
                ent.scale[0] = t["scale"][0];
                ent.scale[1] = t["scale"][1];
                ent.scale[2] = t["scale"][2];
            }
        }

        // Name
        if (je.contains("name")) {
            ent.componentMask |= assetlib::kComp_Name;
            std::string name = je["name"].is_string()
                ? je["name"].get<std::string>() : "Entity";
            auto [off, len] = assetlib::stringTableAppend(cooked.stringTable, name);
            ent.nameOffset = off;
            ent.nameLength = len;
        }

        // MeshRenderer
        if (je.contains("meshRenderer")) {
            ent.componentMask |= assetlib::kComp_MeshRenderer;
            const auto& mr = je["meshRenderer"];
            std::string srcPath = mr.value("sourcePath", std::string{});
            std::string srcType = mr.value("sourceType", std::string{});
            std::string cooked_path = mr.value("cookedPath", std::string{});

            // If no cookedPath in the JSON, try to resolve it now
            if (cooked_path.empty() && !srcPath.empty() && assetLib
                    && !projectRoot.empty()
                    && srcPath.rfind("engine://", 0) != 0) {
                std::error_code ec;
                auto rel = std::filesystem::relative(srcPath, projectRoot, ec);
                if (!ec && !rel.empty()) {
                    auto rec = assetLib->findBySourcePath(rel.generic_string());
                    if (rec && rec->state == assetlib::AssetState::Ready
                            && !rec->cookedPath.empty())
                        cooked_path = rec->cookedPath;
                }
            }

            auto [coff, clen] = assetlib::stringTableAppend(cooked.stringTable, cooked_path);
            ent.meshCookedOffset = coff;
            ent.meshCookedLength = clen;
            auto [soff, slen] = assetlib::stringTableAppend(cooked.stringTable, srcPath);
            ent.meshSourceOffset = soff;
            ent.meshSourceLength = slen;
            ent.meshSourceType   = (srcType == "primitive") ? 1 : 0;
            ent.matOverrideId    = mr.value("matOverrideId", 0u);
        }

        // Camera
        if (je.contains("camera")) {
            ent.componentMask |= assetlib::kComp_Camera;
            const auto& c = je["camera"];
            ent.cameraIsPrimary  = c.value("isPrimary", true) ? 1 : 0;
            ent.cameraProjection = static_cast<uint8_t>(c.value("projection", 0));
            ent.cameraFov        = c.value("fov", 60.0f);
            ent.cameraOrthoSize  = c.value("orthoSize", 10.0f);
            ent.cameraNearPlane  = c.value("nearPlane", 0.1f);
            ent.cameraFarPlane   = c.value("farPlane", 1000.0f);
            if (c.contains("clearColor")) {
                const auto& cc = c["clearColor"];
                ent.cameraClearColor[0] = cc[0]; ent.cameraClearColor[1] = cc[1];
                ent.cameraClearColor[2] = cc[2]; ent.cameraClearColor[3] = cc[3];
            }
        }

        // RigidBody
        if (je.contains("rigidBody")) {
            ent.componentMask |= assetlib::kComp_RigidBody;
            const auto& rb = je["rigidBody"];
            ent.rbBodyType    = static_cast<uint8_t>(rb.value("bodyType", 1));
            ent.rbShape       = static_cast<uint8_t>(rb.value("shape", 0));
            ent.rbMass        = rb.value("mass", 1.0f);
            ent.rbRestitution = rb.value("restitution", 0.3f);
            ent.rbFriction    = rb.value("friction", 0.6f);
            ent.rbUseGravity  = rb.value("useGravity", true) ? 1 : 0;
            if (rb.contains("halfExtent")) {
                const auto& he = rb["halfExtent"];
                ent.rbHalfExtent[0] = he[0]; ent.rbHalfExtent[1] = he[1]; ent.rbHalfExtent[2] = he[2];
            }
            ent.rbRadius     = rb.value("radius", 0.5f);
            ent.rbHalfHeight = rb.value("halfHeight", 0.5f);
        }

        // Script
        if (je.contains("script")) {
            ent.componentMask |= assetlib::kComp_Script;
            std::string path = je["script"].value("path", std::string{});
            auto [off, len] = assetlib::stringTableAppend(cooked.stringTable, path);
            ent.scriptPathOffset = off;
            ent.scriptPathLength = len;
        }

        // CharacterController
        if (je.contains("characterController")) {
            ent.componentMask |= assetlib::kComp_CharacterController;
            const auto& cc = je["characterController"];
            ent.ccRadius      = cc.value("radius", 0.3f);
            ent.ccHeight      = cc.value("height", 1.8f);
            ent.ccMaxSlopeDeg = cc.value("maxSlopeDeg", 45.0f);
            ent.ccStepHeight  = cc.value("stepHeight", 0.3f);
            ent.ccMass        = cc.value("mass", 70.0f);
            ent.ccGravityScale= cc.value("gravityScale", 1.0f);
        }

        // Light
        if (je.contains("light")) {
            ent.componentMask |= assetlib::kComp_Light;
            const auto& l = je["light"];
            ent.lightType        = static_cast<uint8_t>(l.value("type", 0));
            if (l.contains("color")) {
                const auto& c = l["color"];
                ent.lightColor[0] = c[0]; ent.lightColor[1] = c[1]; ent.lightColor[2] = c[2];
            }
            ent.lightIntensity   = l.value("intensity", 3.0f);
            ent.lightRange       = l.value("range", 15.0f);
            ent.lightSpotInner   = l.value("spotInner", 25.0f);
            ent.lightSpotOuter   = l.value("spotOuter", 35.0f);
            ent.lightCastShadows = l.value("castShadows", false) ? 1 : 0;
            ent.lightUseTemp     = l.value("useTemperature", false) ? 1 : 0;
            ent.lightTemperatureK= l.value("temperatureK", 6500.0f);
        }

        cooked.entities.push_back(ent);
    }

    if (!assetlib::saveScene(cooked, outPath)) {
        LOG_ERROR("SceneCook", "Failed to write binary scene: %s", outPath.string().c_str());
        return false;
    }

    LOG_SUCCESS("SceneCook", "Cooked %zu entities → %s (string table: %zu bytes)",
                cooked.entities.size(), outPath.filename().string().c_str(),
                cooked.stringTable.size());
    return true;
}

} // namespace SceneSerializer
