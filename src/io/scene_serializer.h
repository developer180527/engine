#pragma once
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
#include "components/camera.h"
#include "engine/logger.h"

namespace SceneSerializer {

// Save persistent entities to disk.
inline bool save(const std::filesystem::path& path,
                 flecs::world& ecs, AssetRegistry& assets) {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();

    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name& n, const Transform& t) {
            if (e.has<Spinner>()) return; // skip procedural entities

            nlohmann::json je;
            je["name"] = n.value;
            je["transform"]["position"] = {t.position.x, t.position.y, t.position.z};
            je["transform"]["rotation"] = {t.rotation.x, t.rotation.y,
                                           t.rotation.z, t.rotation.w};
            je["transform"]["scale"]    = {t.scale.x, t.scale.y, t.scale.z};

            if (const MeshRenderer* mr = e.try_get<MeshRenderer>()) {
                je["meshRenderer"]["handleId"] = mr->mesh.id;
                const Mesh* mesh = assets.getMesh(mr->mesh);
                if (mesh && !mesh->sourcePath.empty())
                    je["meshRenderer"]["sourcePath"] = mesh->sourcePath;
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
                      ImporterRegistry& importers) {
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
                        ecs.entity(name.c_str())
                            .set<Transform>(t)
                            .set<Name>({name})
                            .set<MeshRenderer>({result.mesh});
                        LOG_SUCCESS("Scene", "Loaded glTF: %s",
                                    std::filesystem::path(assetPath).filename().string().c_str());
                    } else {
                        LOG_ERROR("Scene", "glTF load failed: %s", result.error.c_str());
                        ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
                    }
                } else {
                    // Async — Assimp handles FBX/OBJ/DAE/etc on worker thread
                    auto e = ecs.entity(name.c_str())
                                .set<Transform>(t)
                                .set<Name>({name});

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
            ecs.entity(name.c_str()).set<Transform>(t).set<Name>({name});
        }
    }

    LOG_INFO("Scene", "Queued %d assets for async load → %s",
             queued, scenePath.filename().string().c_str());
    return true;
}

// ── In-memory snapshot: save editor world to JSON string ──────────────────
// Stores handle IDs alongside source paths so game world can reuse
// live asset handles without any re-loading (instant game world creation).
inline std::string saveToString(flecs::world& ecs, AssetStorage& assets) {
    nlohmann::json scene;
    scene["version"]  = 1;
    scene["entities"] = nlohmann::json::array();
    ecs.query_builder<const Name, const Transform>().build()
        .each([&](flecs::entity e, const Name& n, const Transform& t) {
            if (e.has<Spinner>()) return;
            nlohmann::json je;
            je["name"] = n.value;
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
        ++count;
    }
    LOG_INFO("Scene", "Game world populated: %d entities (instant, shared handles)", count);
}

} // namespace SceneSerializer
