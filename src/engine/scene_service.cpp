#include "engine/scene_service.h"

#include "engine/asset_service.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/primitive_library.h"
#include "engine/logger.h"

#include "core/transform.h"
#include "core/entity_id_util.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/light.h"

#include <assetlib/scene_asset.h>

#include <flecs.h>
#include <algorithm>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════

SceneService::SceneService(Config cfg)
    : m_assets(cfg.assets)
    , m_meshes(cfg.meshes)
    , m_textures(cfg.textures)
    , m_materials(cfg.materials)
    , m_world(cfg.world)
    , m_primitives(cfg.primitives)
{}

SceneService::~SceneService() = default;

// ═══════════════════════════════════════════════════════════════════════════
// Path helper
// ═══════════════════════════════════════════════════════════════════════════

std::string SceneService::resolvePath(const char* cookedPath) const {
    std::filesystem::path p(cookedPath);
    if (p.is_relative() && !m_cacheRoot.empty())
        p = m_cacheRoot / p;
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════
// Sync loadScene
// ═══════════════════════════════════════════════════════════════════════════

uint32_t SceneService::loadScene(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return 0;

    std::string absPath = resolvePath(cookedPath);

    // Load the binary scene
    assetlib::SceneAsset scene;
    if (!assetlib::loadScene(scene, absPath)) {
        LOG_ERROR("SceneService", "Failed to load binary scene: %s", cookedPath);
        return 0;
    }

    const auto& st = scene.stringTable;
    LoadedScene loaded;
    loaded.path = absPath;

    // First pass: create all entities with identity + name + transform.
    // This must happen before parent links since children need parents to exist.
    struct EntityInfo {
        flecs::entity entity;
        uint64_t      parentId;
        size_t        idx;  // index into scene.entities
    };
    std::vector<EntityInfo> created;
    std::unordered_map<uint64_t, flecs::entity> byId;

    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const auto& se = scene.entities[i];
        flecs::entity e = m_world.entity();

        // Identity
        uint64_t id = se.entityId;
        if (id == 0 || findById(m_world, id).is_alive()) {
            if (id != 0)
                LOG_WARN("SceneService", "EntityId %llu already live — reassigning",
                         (unsigned long long)id);
            do { id = generateEntityId(); } while (findById(m_world, id).is_alive());
        }
        e.set<EntityId>({id});
        byId[id] = e;

        // Name
        if (se.componentMask & assetlib::kComp_Name) {
            std::string name = assetlib::stringTableRead(st, se.nameOffset, se.nameLength);
            if (!name.empty())
                e.set<Name>({name});
        }

        // Transform
        if (se.componentMask & assetlib::kComp_Transform) {
            Transform t;
            t.position = {se.position[0], se.position[1], se.position[2]};
            t.rotation = {se.rotation[0], se.rotation[1], se.rotation[2], se.rotation[3]};
            t.scale    = {se.scale[0], se.scale[1], se.scale[2]};
            e.set<Transform>(t);
        }

        // MeshRenderer
        if (se.componentMask & assetlib::kComp_MeshRenderer) {
            MeshHandle mh{};

            // Try cooked path first (fast runtime path)
            std::string meshCooked = assetlib::stringTableRead(
                st, se.meshCookedOffset, se.meshCookedLength);
            if (!meshCooked.empty()) {
                mh = m_assets.loadMesh(meshCooked.c_str());
            }

            // Fallback: primitive
            if (!mh.valid() && se.meshSourceType == 1 && m_primitives && m_primitives->ready()) {
                std::string srcPath = assetlib::stringTableRead(
                    st, se.meshSourceOffset, se.meshSourceLength);
                if (!srcPath.empty()) {
                    std::string primName = srcPath.substr(srcPath.rfind('/') + 1);
                    mh = m_primitives->byName(primName);
                }
            }

            if (mh.valid()) {
                MeshRenderer mr{mh};
                if (se.matOverrideId > 0) mr.materialOverride.id = se.matOverrideId;
                e.set<MeshRenderer>(mr);
                loaded.meshHandles.push_back(mh);
            } else {
                std::string src = assetlib::stringTableRead(
                    st, se.meshSourceOffset, se.meshSourceLength);
                LOG_WARN("SceneService", "Could not load mesh for entity %llu: cooked='%s' source='%s'",
                         (unsigned long long)id, meshCooked.c_str(), src.c_str());
            }
        }

        // Camera
        if (se.componentMask & assetlib::kComp_Camera) {
            Camera cam;
            cam.isPrimary  = se.cameraIsPrimary != 0;
            cam.projection = static_cast<ProjectionType>(se.cameraProjection);
            cam.fov        = se.cameraFov;
            cam.orthoSize  = se.cameraOrthoSize;
            cam.nearPlane  = se.cameraNearPlane;
            cam.farPlane   = se.cameraFarPlane;
            cam.clearColor[0] = se.cameraClearColor[0];
            cam.clearColor[1] = se.cameraClearColor[1];
            cam.clearColor[2] = se.cameraClearColor[2];
            cam.clearColor[3] = se.cameraClearColor[3];
            e.set<Camera>(cam);
        }

        // RigidBody
        if (se.componentMask & assetlib::kComp_RigidBody) {
            RigidBody rb;
            rb.bodyType    = static_cast<PhysicsBodyType>(se.rbBodyType);
            rb.shape       = static_cast<PhysicsShape>(se.rbShape);
            rb.mass        = se.rbMass;
            rb.restitution = se.rbRestitution;
            rb.friction    = se.rbFriction;
            rb.useGravity  = se.rbUseGravity != 0;
            rb.halfExtent  = {se.rbHalfExtent[0], se.rbHalfExtent[1], se.rbHalfExtent[2]};
            rb.radius      = se.rbRadius;
            rb.halfHeight  = se.rbHalfHeight;
            e.set<RigidBody>(rb);
        }

        // Script
        if (se.componentMask & assetlib::kComp_Script) {
            std::string path = assetlib::stringTableRead(
                st, se.scriptPathOffset, se.scriptPathLength);
            ScriptComponent sc;
            sc.scriptPath = path;
            e.set<ScriptComponent>(sc);
        }

        // CharacterController
        if (se.componentMask & assetlib::kComp_CharacterController) {
            CharacterController cc;
            cc.radius       = se.ccRadius;
            cc.height       = se.ccHeight;
            cc.maxSlopeDeg  = se.ccMaxSlopeDeg;
            cc.stepHeight   = se.ccStepHeight;
            cc.mass         = se.ccMass;
            cc.gravityScale = se.ccGravityScale;
            e.set<CharacterController>(cc);
        }

        // Light
        if (se.componentMask & assetlib::kComp_Light) {
            Light l;
            l.type           = static_cast<LightType>(se.lightType);
            l.color          = {se.lightColor[0], se.lightColor[1], se.lightColor[2]};
            l.intensity      = se.lightIntensity;
            l.range          = se.lightRange;
            l.spotInner      = se.lightSpotInner;
            l.spotOuter      = se.lightSpotOuter;
            l.castShadows    = se.lightCastShadows != 0;
            l.useTemperature = se.lightUseTemp != 0;
            l.temperatureK   = se.lightTemperatureK;
            e.set<Light>(l);
        }

        loaded.entityIds.push_back(e.id());
        created.push_back({e, se.parentId, i});
    }

    // Second pass: restore parent links
    for (auto& info : created) {
        if (info.parentId == 0) continue;
        // Look up by the ORIGINAL parentId from the scene file
        // We need to find which entity got that original ID (or reassigned ID)
        // Since we stored the final ID in byId keyed by final ID, we need another map
        // Actually, let's build a map from original scene ID → entity
    }
    // Build original-ID → entity map
    std::unordered_map<uint64_t, flecs::entity> byOriginalId;
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        byOriginalId[scene.entities[i].entityId] = created[i].entity;
    }
    for (auto& info : created) {
        if (info.parentId == 0) continue;
        auto pit = byOriginalId.find(info.parentId);
        if (pit == byOriginalId.end()) continue;
        flecs::entity child  = info.entity;
        flecs::entity parent = pit->second;
        if (child.is_alive() && parent.is_alive() && child != parent)
            child.add(flecs::ChildOf, parent);
    }

    uint32_t handle = m_nextHandle++;
    m_scenes[handle] = std::move(loaded);

    LOG_SUCCESS("SceneService", "Loaded binary scene: %s (%zu entities, handle=%u)",
                cookedPath, scene.entities.size(), handle);
    return handle;
}

// ═══════════════════════════════════════════════════════════════════════════
// Unload
// ═══════════════════════════════════════════════════════════════════════════

bool SceneService::unloadScene(uint32_t sceneHandle) {
    auto it = m_scenes.find(sceneHandle);
    if (it == m_scenes.end()) return false;

    auto& loaded = it->second;

    // Destroy entities (children are auto-destroyed by flecs ChildOf)
    for (auto eid : loaded.entityIds) {
        flecs::entity e = m_world.entity(eid);
        if (e.is_alive())
            e.destruct();
    }

    // Unload owned assets
    for (auto h : loaded.meshHandles)
        m_assets.unloadMesh(h);

    LOG_INFO("SceneService", "Unloaded scene handle=%u (%zu entities, %zu meshes)",
             sceneHandle, loaded.entityIds.size(), loaded.meshHandles.size());

    m_scenes.erase(it);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Async preload
// ═══════════════════════════════════════════════════════════════════════════

void SceneService::preloadScene(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return;

    std::string absPath = resolvePath(cookedPath);

    // Already preloading?
    if (m_preloads.count(absPath)) return;

    assetlib::SceneAsset scene;
    if (!assetlib::loadScene(scene, absPath)) {
        LOG_ERROR("SceneService", "preloadScene: failed to parse: %s", cookedPath);
        return;
    }

    PreloadEntry entry;
    const auto& st = scene.stringTable;

    for (const auto& se : scene.entities) {
        if (!(se.componentMask & assetlib::kComp_MeshRenderer)) continue;
        std::string meshCooked = assetlib::stringTableRead(
            st, se.meshCookedOffset, se.meshCookedLength);
        if (!meshCooked.empty()) {
            m_assets.loadMeshAsync(meshCooked.c_str());
            entry.meshPaths.push_back(meshCooked);
        }
    }

    m_preloads[absPath] = std::move(entry);
    LOG_INFO("SceneService", "Preloading scene: %s (%zu meshes queued)",
             cookedPath, m_preloads[absPath].meshPaths.size());
}

bool SceneService::isSceneReady(const char* cookedPath) const {
    if (!cookedPath) return false;
    std::string absPath = resolvePath(cookedPath);
    auto it = m_preloads.find(absPath);
    if (it == m_preloads.end()) return false;

    // Check if all queued meshes have finished loading
    for (const auto& path : it->second.meshPaths) {
        if (m_assets.queryMesh(path.c_str()) == 0
            && m_assets.isLoading(path.c_str()))
            return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Query
// ═══════════════════════════════════════════════════════════════════════════

uint32_t SceneService::sceneEntityCount(uint32_t sceneHandle) const {
    auto it = m_scenes.find(sceneHandle);
    return it != m_scenes.end()
        ? static_cast<uint32_t>(it->second.entityIds.size()) : 0;
}

uint32_t SceneService::activeSceneCount() const {
    return static_cast<uint32_t>(m_scenes.size());
}
