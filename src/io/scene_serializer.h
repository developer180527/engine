#pragma once
#include "core/transform_utils.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include <flecs.h>
#include <bx/math.h>

#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "io/asset_storage.h"
#include "io/importer_registry.h"
#include "engine/async_loader.h"
#include "render/primitive_library.h"
#include "render/primitive_library.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "core/entity_id_util.h"
#include <unordered_map>
#include "engine/logger.h"

namespace SceneSerializer {

// Save persistent entities to disk.
inline bool save(const std::filesystem::path& path,
                 flecs::world& ecs, AssetRegistry& assets) {
    std::filesystem::create_directories(path.parent_path());
    assignMissingIds(ecs);   // every saved entity gets a stable id
    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();

    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name& n, const Transform& t) {
            if (e.has<Spinner>()) return; // skip procedural entities

            nlohmann::json je;
            je["name"] = n.value;
            // Stable identity + parent reference (by id, not mutable name)
            if (const EntityId* myId = e.try_get<EntityId>())
                je["id"] = myId->value;
            flecs::entity par = e.target(flecs::ChildOf);
            if (par && par.is_alive())
                if (const EntityId* pid = par.try_get<EntityId>())
                    je["parentId"] = pid->value;
            je["transform"]["position"] = {t.position.x, t.position.y, t.position.z};
            je["transform"]["rotation"] = {t.rotation.x, t.rotation.y,
                                           t.rotation.z, t.rotation.w};
            je["transform"]["scale"]    = {t.scale.x, t.scale.y, t.scale.z};

            if (const MeshRenderer* mr = e.try_get<MeshRenderer>()) {
                je["meshRenderer"]["handleId"] = mr->mesh.id;
                const Mesh* mesh = assets.getMesh(mr->mesh);
                if (mesh && !mesh->sourcePath.empty()) {
                    je["meshRenderer"]["sourcePath"] = mesh->sourcePath;
                    if (mesh->sourcePath.rfind("engine://primitive/", 0) == 0)
                        je["meshRenderer"]["sourceType"] = "primitive";
                }
                if (mr->materialOverride.valid())
                    je["meshRenderer"]["matOverrideId"] = mr->materialOverride.id;
            }
            if (const Camera* cam = e.try_get<Camera>()) {
                je["camera"]["isPrimary"]  = cam->isPrimary;
                je["camera"]["projection"] = (int)cam->projection;
                je["camera"]["fov"]        = cam->fov;
                je["camera"]["orthoSize"]  = cam->orthoSize;
                je["camera"]["nearPlane"]  = cam->nearPlane;
                je["camera"]["farPlane"]   = cam->farPlane;
                je["camera"]["clearColor"] = {cam->clearColor[0], cam->clearColor[1],
                                               cam->clearColor[2], cam->clearColor[3]};
            }
            if (const RigidBody* rb = e.try_get<RigidBody>()) {
                je["rigidBody"]["bodyType"]    = (int)rb->bodyType;
                je["rigidBody"]["shape"]       = (int)rb->shape;
                je["rigidBody"]["mass"]        = rb->mass;
                je["rigidBody"]["restitution"] = rb->restitution;
                je["rigidBody"]["friction"]    = rb->friction;
                je["rigidBody"]["useGravity"]  = rb->useGravity;
                je["rigidBody"]["halfExtent"]  = {rb->halfExtent.x, rb->halfExtent.y, rb->halfExtent.z};
                je["rigidBody"]["radius"]      = rb->radius;
                je["rigidBody"]["halfHeight"]  = rb->halfHeight;
            }
            if (const ScriptComponent* sc = e.try_get<ScriptComponent>())
                if (!sc->scriptPath.empty())
                    je["script"]["path"] = sc->scriptPath;
            scene["entities"].push_back(je);
        });

    std::ofstream f(path);
    f << scene.dump(2);
    LOG_SUCCESS("Scene", "Saved %zu entities → %s",
                scene["entities"].size(), path.filename().string().c_str());
    return f.good();
}

// Load scene asynchronously — parses JSON immediately (fast),
// submits each asset to the async loader, spawns entities when ready.
// Engine is fully interactive while assets stream in.
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

    // Restore optional components (Camera, RigidBody) onto any entity.
    // Called after entity creation in every load path — these components
    // are immediate and don't depend on async mesh loading.
    auto restoreComponents = [&](flecs::entity ent, const nlohmann::json& je) {
        uint64_t eid = je.value("id", (uint64_t)0);
        ent.set<EntityId>({ eid != 0 ? eid : generateEntityId() });
        if (je.contains("camera")) {
            const auto& jc = je["camera"];
            Camera cam;
            cam.isPrimary  = jc.value("isPrimary",  true);
            cam.projection = (ProjectionType)jc.value("projection", 0);
            cam.fov        = jc.value("fov",        60.0f);
            cam.orthoSize  = jc.value("orthoSize",  10.0f);
            cam.nearPlane  = jc.value("nearPlane",  0.1f);
            cam.farPlane   = jc.value("farPlane",   1000.0f);
            if (jc.contains("clearColor")) {
                const auto& cc = jc["clearColor"];
                cam.clearColor[0]=cc[0]; cam.clearColor[1]=cc[1];
                cam.clearColor[2]=cc[2]; cam.clearColor[3]=cc[3];
            }
            ent.set<Camera>(cam);
        }
        if (je.contains("rigidBody")) {
            const auto& jrb = je["rigidBody"];
            RigidBody rb;
            rb.bodyType    = (PhysicsBodyType)jrb.value("bodyType",    1);
            rb.shape       = (PhysicsShape)   jrb.value("shape",       0);
            rb.mass        = jrb.value("mass",        1.0f);
            rb.restitution = jrb.value("restitution", 0.3f);
            rb.friction    = jrb.value("friction",    0.6f);
            rb.useGravity  = jrb.value("useGravity",  true);
            if (jrb.contains("halfExtent")) {
                const auto& he = jrb["halfExtent"];
                rb.halfExtent = {he[0], he[1], he[2]};
            }
            rb.radius     = jrb.value("radius",     0.5f);
            rb.halfHeight = jrb.value("halfHeight", 0.5f);
            ent.set<RigidBody>(rb);
        }
        if (je.contains("script")) {
            ScriptComponent sc;
            sc.scriptPath = je["script"].value("path", "");
            ent.set<ScriptComponent>(sc);
        }
    };

    int queued = 0;
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        std::string name = je.value("name", "Entity");

        // Read transform — always immediate
        Transform t;
        if (je.contains("transform")) {
            const auto& jt = je["transform"];
            if (jt.contains("position"))
                t.position = {jt["position"][0], jt["position"][1], jt["position"][2]};
            if (jt.contains("rotation"))
                t.rotation = {jt["rotation"][0], jt["rotation"][1],
                              jt["rotation"][2], jt["rotation"][3]};
            if (jt.contains("scale"))
                t.scale = {jt["scale"][0], jt["scale"][1], jt["scale"][2]};
        }

        if (je.contains("meshRenderer")) {
            // ── Primitive mesh shortcut ───────────────────────────────────
            std::string srcPath0 = je["meshRenderer"].value("sourcePath", "");
            std::string sourceType = je["meshRenderer"].value("sourceType", "");
            bool isPrimitive = (sourceType == "primitive") ||
                               (srcPath0.rfind("engine://primitive/", 0) == 0);
            if (isPrimitive && primitives && primitives->ready()) {
                std::string srcPath = srcPath0;
                std::string primName = srcPath.substr(srcPath.rfind('/')+1);
                MeshHandle h = primitives->byName(primName);
                if (h.valid()) {
                    { auto ent = ecs.entity(name.c_str())
                        .set<Transform>(t).set<Name>({name})
                        .set<MeshRenderer>({h});
                      restoreComponents(ent, je); }
                    LOG_SUCCESS("Scene", "Primitive: %s (%s)", name.c_str(), primName.c_str());
                    continue;
                }
            }
            std::string assetPath = je["meshRenderer"].value("sourcePath", "");
            if (!assetPath.empty()) {
                // Route by extension:
                // glTF/GLB → ImporterRegistry (cgltf, main-thread safe)
                // FBX/OBJ/etc → AsyncLoader (Assimp, worker thread)
                std::string ext = std::filesystem::path(assetPath).extension().string();
                for (auto& c : ext) c = (char)std::tolower(c);
                const bool isGltf = (ext == ".glb" || ext == ".gltf");

                if (isGltf) {
                    // Synchronous — cgltf is fast and main-thread only
                    auto result = importers.loadCached(assetPath, storage);
                    if (result.success) {
                        if (Mesh* m = const_cast<Mesh*>(storage.meshes.getMesh(result.mesh)))
                            m->sourcePath = assetPath;
                        { auto ent = ecs.entity(name.c_str())
                            .set<Transform>(t)
                            .set<Name>({name})
                            .set<MeshRenderer>({result.mesh});
                          restoreComponents(ent, je); }
                        LOG_SUCCESS("Scene", "Loaded glTF: %s",
                                    std::filesystem::path(assetPath).filename().string().c_str());
                    } else {
                        LOG_ERROR("Scene", "glTF load failed: %s", result.error.c_str());
                        { auto ent = ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
                          restoreComponents(ent, je); }
                    }
                } else {
                    // Async — Assimp handles FBX/OBJ/DAE/etc on worker thread
                    auto e = ecs.entity(name.c_str())
                                .set<Transform>(t)
                                .set<Name>({name});
                    restoreComponents(e, je);

                    flecs::entity_t eid = e.id();
                    flecs::world*   pw  = &ecs;

                    loader.load(assetPath, name,
                        [pw, eid](MeshHandle h, const std::string&) {
                            if (!h.valid()) return;
                            flecs::entity e = pw->entity(eid);
                            if (e.id() != 0 && e.is_alive())
                                e.set<MeshRenderer>({h});
                        });
                    ++queued;
                }
            }
        } else {
            // Entity with no mesh — spawn immediately
            { auto ent = ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
              restoreComponents(ent, je); }
        }
    }

    LOG_INFO("Scene", "Queued %d assets for async load → %s",
             queued, scenePath.filename().string().c_str());
    // ── Restore parent relationships (by stable id) ───────────────────
    // Resolve child/parent by EntityId; fall back to name for legacy
    // scenes that predate ids.
    {
        std::unordered_map<uint64_t, flecs::entity> byId;
        ecs.query_builder<const EntityId>().build()
            .each([&](flecs::entity e, const EntityId& id) { byId[id.value] = e; });
        for (const auto& je : scene.value("entities", nlohmann::json::array())) {
            uint64_t cid = je.value("id", (uint64_t)0);
            uint64_t pid = je.value("parentId", (uint64_t)0);
            flecs::entity child  = (cid && byId.count(cid)) ? byId[cid] : flecs::entity{};
            flecs::entity parent = (pid && byId.count(pid)) ? byId[pid] : flecs::entity{};
            if (!child.is_alive() && je.contains("name"))
                child = ecs.lookup(je.value("name", "").c_str());
            if (!parent.is_alive() && je.contains("parent"))
                parent = ecs.lookup(je.value("parent", "").c_str());
            if (child.is_alive() && parent.is_alive() && child != parent
                && !isAncestorOf(child, parent))
                child.add(flecs::ChildOf, parent);
        }
    }
    return true;
}

// ── In-memory snapshot: save editor world to JSON string ──────────────────
// Stores handle IDs alongside source paths so game world can reuse
// live asset handles without any re-loading (instant game world creation).
inline std::string saveToString(flecs::world& ecs, AssetStorage& assets) {
    assignMissingIds(ecs);   // every snapshotted entity gets a stable id
    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();
    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name& n, const Transform& t) {
            if (e.has<Spinner>()) return;
            nlohmann::json je;
            je["name"] = n.value;
            // Stable identity + parent reference (by id, not mutable name)
            if (const EntityId* myId = e.try_get<EntityId>())
                je["id"] = myId->value;
            flecs::entity par = e.target(flecs::ChildOf);
            if (par && par.is_alive())
                if (const EntityId* pid = par.try_get<EntityId>())
                    je["parentId"] = pid->value;
            je["transform"]["position"] = {t.position.x, t.position.y, t.position.z};
            je["transform"]["rotation"] = {t.rotation.x, t.rotation.y,
                                           t.rotation.z, t.rotation.w};
            je["transform"]["scale"]    = {t.scale.x, t.scale.y, t.scale.z};
            if (const MeshRenderer* mr = e.try_get<MeshRenderer>()) {
                je["meshRenderer"]["handleId"] = mr->mesh.id;
                if (mr->materialOverride.valid())
                    je["meshRenderer"]["matOverrideId"] = mr->materialOverride.id;
                const Mesh* mesh = assets.meshes.getMesh(mr->mesh);
                if (mesh && !mesh->sourcePath.empty())
                    je["meshRenderer"]["sourcePath"] = mesh->sourcePath;
            }
            if (const Camera* cam = e.try_get<Camera>()) {
                je["camera"]["isPrimary"]  = cam->isPrimary;
                je["camera"]["projection"] = (int)cam->projection;
                je["camera"]["fov"]        = cam->fov;
                je["camera"]["orthoSize"]  = cam->orthoSize;
                je["camera"]["nearPlane"]  = cam->nearPlane;
                je["camera"]["farPlane"]   = cam->farPlane;
                je["camera"]["clearColor"] = {cam->clearColor[0], cam->clearColor[1],
                                              cam->clearColor[2], cam->clearColor[3]};
            }
            if (const RigidBody* rb = e.try_get<RigidBody>()) {
                je["rigidBody"]["bodyType"]    = (int)rb->bodyType;
                je["rigidBody"]["shape"]       = (int)rb->shape;
                je["rigidBody"]["mass"]        = rb->mass;
                je["rigidBody"]["restitution"] = rb->restitution;
                je["rigidBody"]["friction"]    = rb->friction;
                je["rigidBody"]["useGravity"]  = rb->useGravity;
                je["rigidBody"]["halfExtent"]  = {rb->halfExtent.x, rb->halfExtent.y, rb->halfExtent.z};
                je["rigidBody"]["radius"]      = rb->radius;
                je["rigidBody"]["halfHeight"]  = rb->halfHeight;
            }
            if (const ScriptComponent* sc = e.try_get<ScriptComponent>())
                if (!sc->scriptPath.empty())
                    je["script"]["path"] = sc->scriptPath;
            scene["entities"].push_back(je);
        });
    return scene.dump();
}

// ── Instant game world population from snapshot string ────────────────────
// Uses stored handle IDs directly — assets already loaded, zero re-parse.
inline void loadIntoWorld(const std::string& snapshot, flecs::world& world,
                          AssetStorage& assets) {
    nlohmann::json scene;
    try { scene = nlohmann::json::parse(snapshot); }
    catch (const std::exception& ex) {
        LOG_ERROR("Scene", "loadIntoWorld parse error: %s", ex.what());
        return;
    }
    int count = 0;
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        std::string name = je.value("name", "Entity");
        Transform t;
        if (je.contains("transform")) {
            const auto& jt = je["transform"];
            if (jt.contains("position"))
                t.position = {jt["position"][0], jt["position"][1], jt["position"][2]};
            if (jt.contains("rotation"))
                t.rotation = {jt["rotation"][0], jt["rotation"][1],
                              jt["rotation"][2], jt["rotation"][3]};
            if (jt.contains("scale"))
                t.scale = {jt["scale"][0], jt["scale"][1], jt["scale"][2]};
        }
        auto e = world.entity(name.c_str()).set<Transform>(t).set<Name>({name});
        { uint64_t eid = je.value("id", (uint64_t)0);
          e.set<EntityId>({ eid != 0 ? eid : generateEntityId() }); }
        if (je.contains("meshRenderer")) {
            uint32_t hid = je["meshRenderer"].value("handleId", 0u);
            if (hid > 0) {
                MeshHandle mh; mh.id = hid;
                MeshRenderer mr{mh};
                if (je["meshRenderer"].contains("matOverrideId")) {
                    mr.materialOverride.id = je["meshRenderer"]["matOverrideId"];
                }
                e.set<MeshRenderer>(mr);
            }
        }
        if (je.contains("camera")) {
            Camera cam;
            cam.isPrimary  = je["camera"].value("isPrimary",  true);
            cam.projection = (ProjectionType)je["camera"].value("projection", 0);
            cam.fov        = je["camera"].value("fov",        60.0f);
            cam.orthoSize  = je["camera"].value("orthoSize",  10.0f);
            cam.nearPlane  = je["camera"].value("nearPlane",  0.1f);
            cam.farPlane   = je["camera"].value("farPlane",   1000.0f);
            if (je["camera"].contains("clearColor")) {
                const auto& cc = je["camera"]["clearColor"];
                cam.clearColor[0]=cc[0]; cam.clearColor[1]=cc[1];
                cam.clearColor[2]=cc[2]; cam.clearColor[3]=cc[3];
            }
            e.set<Camera>(cam);
        }
        if (je.contains("rigidBody")) {
            const auto& jrb = je["rigidBody"];
            RigidBody rb;
            rb.bodyType    = (PhysicsBodyType)jrb.value("bodyType",    1);
            rb.shape       = (PhysicsShape)   jrb.value("shape",       0);
            rb.mass        = jrb.value("mass",        1.0f);
            rb.restitution = jrb.value("restitution", 0.3f);
            rb.friction    = jrb.value("friction",    0.6f);
            rb.useGravity  = jrb.value("useGravity",  true);
            if (jrb.contains("halfExtent")) {
                const auto& he = jrb["halfExtent"];
                rb.halfExtent = {he[0], he[1], he[2]};
            }
            rb.radius     = jrb.value("radius",     0.5f);
            rb.halfHeight = jrb.value("halfHeight", 0.5f);
            e.set<RigidBody>(rb);
        }
        if (je.contains("script")) {
            ScriptComponent sc;
            sc.scriptPath = je["script"].value("path", "");
            e.set<ScriptComponent>(sc);
        }
        ++count;
    }
    // Restore parent relationships in game world (by stable id)
    {
        std::unordered_map<uint64_t, flecs::entity> byId;
        world.query_builder<const EntityId>().build()
            .each([&](flecs::entity e, const EntityId& id) { byId[id.value] = e; });
        for (const auto& je : scene.value("entities", nlohmann::json::array())) {
            uint64_t cid = je.value("id", (uint64_t)0);
            uint64_t pid = je.value("parentId", (uint64_t)0);
            flecs::entity child  = (cid && byId.count(cid)) ? byId[cid] : flecs::entity{};
            flecs::entity parent = (pid && byId.count(pid)) ? byId[pid] : flecs::entity{};
            if (!child.is_alive() && je.contains("name"))
                child = world.lookup(je.value("name", "").c_str());
            if (!parent.is_alive() && je.contains("parent"))
                parent = world.lookup(je.value("parent", "").c_str());
            if (child.is_alive() && parent.is_alive() && child != parent
                && !isAncestorOf(child, parent))
                child.add(flecs::ChildOf, parent);
        }
    }
    LOG_INFO("Scene", "Game world populated: %d entities (instant, shared handles)", count);
}

} // namespace SceneSerializer
