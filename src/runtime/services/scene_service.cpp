#include "runtime/services/scene_service.h"

#include "runtime/services/asset_service.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/primitive_library.h"
#include "core/logger.h"

#include "core/transform.h"
#include "components/transform_hierarchy.h"   // safeReparent (depth/cycle-guarded)
#include "components/entity_id_util.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/light.h"
#include "components/skinned_mesh.h"
#include "components/lod_mesh.h"      // cooked LOD chain -> LodMesh
#include "render/world/lod.h"          // kMaxLodLevels
#include "components/animator.h"
#include "animation/clip_library.h"        // standalone clipPath binding
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"

#include <assetlib/scene_asset.h>
#include <nlohmann/json.hpp>

#include "core/json_read.h"
#include "assets/cooked_scene.h"
#include "scene/entity_serializer.h"
#include "scene/scene_assets.h"

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
    , m_clipLibrary(cfg.clipLibrary)
    , m_skeletons(cfg.skeletons)
    , m_clips(cfg.clips)
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
    const std::string absPath = resolvePath(cookedPath);

    // ── The cooked scene is the scene's own saved form (WO-091) ─────────────
    // Read through the schema registry, exactly as the editor reads a .scene, so
    // every component the scene holds reaches the game: colour grading, kits'
    // components, components from a provider this build lacks (kept, DR-0034).
    // This replaced a fixed 256-byte record with slots for nine components.
    assetlib::SceneAsset file;
    std::string why;
    nlohmann::json scene;
    if (!assetlib::loadScene(file, absPath, &why) || !cooked_scene::decode(file, scene, why)) {
        LOG_ERROR("SceneService", "Failed to load scene %s: %s", cookedPath, why.c_str());
        return 0;
    }

    LoadedScene loaded;
    loaded.path = absPath;

    // Assets through this service, every mesh handle recorded for unload.
    SceneAssets hooks;
    hooks.loadCookedMesh = [this, &loaded](const char* path, std::vector<MeshHandle>* lods,
                                           StreamedMesh* skin) {
        AssetService::MeshLods l;
        AssetService::MeshSkin s;
        const MeshHandle h = m_assets.loadMesh(path, &s, &l);
        if (h.valid()) loaded.meshHandles.push_back(h);
        for (MeshHandle lh : l.levels) loaded.meshHandles.push_back(lh);
        if (lods) *lods = l.levels;
        if (skin) { skin->mesh = h; skin->skeleton = s.skeleton; skin->clips = s.clips; }
        return h;
    };
    hooks.loadMaterial = [this](const char* name) { return m_assets.loadMaterialAsset(name); };
    hooks.bindClip = clipBinder(m_clipLibrary, m_skeletons, m_clips);

    EntitySerde::SerdeContext ctx;
    ctx.mode        = EntitySerde::SerdeMode::Disk;
    ctx.sceneAssets = &hooks;
    ctx.primitives  = m_primitives;
    // Collision checks through the index, not findById: findById is a full
    // query, and per entity it made a scene load quadratic.
    EntityIdIndex ids;
    ids.build(m_world);
    ctx.idIndex = &ids;
    std::vector<EntitySerde::SerdeContext::EntityRef> refs;
    ctx.entityRefs = &refs;

    // Pass 1: every entity. Pass 2: parents, by the ids AS WRITTEN (a colliding
    // id is reassigned on load, so the file's ids are what links refer to).
    struct Created { flecs::entity entity; uint64_t fileId, parentId; };
    std::vector<Created> created;
    const nlohmann::json& entities = scene["entities"];
    created.reserve(entities.size());
    for (const nlohmann::json& je : entities) {
        if (!je.is_object()) continue;   // not an entity: skipped, as the editor does
        flecs::entity e = EntitySerde::createEntity(m_world, je, ctx,
                                                    EntitySerde::IdPolicy::Preserve);
        loaded.entityIds.push_back(e.id());
        created.push_back({e, jsonread::readU64(je, "id", 0), jsonread::readU64(je, "parentId", 0)});
    }
    std::unordered_map<uint64_t, flecs::entity> byFileId;
    byFileId.reserve(created.size());
    for (const Created& c : created)
        if (c.fileId) byFileId.emplace(c.fileId, c.entity);   // first claimant wins
    for (const Created& c : created) {
        if (!c.parentId) continue;
        auto it = byFileId.find(c.parentId);
        if (it == byFileId.end()) continue;
        safeReparent(c.entity, it->second);   // cycle + depth guarded
    }
    EntitySerde::resolveEntityRefs(m_world, ctx);

    const uint32_t handle = m_nextHandle++;
    const size_t count = loaded.entityIds.size();
    m_scenes[handle] = std::move(loaded);
    LOG_SUCCESS("SceneService", "Loaded scene: %s (%zu entities, handle=%u)",
                cookedPath, count, handle);
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

    assetlib::SceneAsset file;
    std::string why;
    nlohmann::json scene;
    if (!assetlib::loadScene(file, absPath, &why) || !cooked_scene::decode(file, scene, why)) {
        LOG_ERROR("SceneService", "preloadScene: %s: %s", cookedPath, why.c_str());
        return;
    }

    PreloadEntry entry;
    for (std::string& mesh : cooked_scene::meshPaths(scene)) {
        m_assets.loadMeshAsync(mesh.c_str());
        entry.meshPaths.push_back(std::move(mesh));
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

    // Ready means EVERY mesh actually has a handle. The old check ("no
    // handle AND still loading") let a FAILED load fall through as ready —
    // no handle, no longer loading, silently missing forever (audit H.6).
    // Callers distinguish "still loading" from "doomed" via
    // sceneLoadFailed().
    for (const auto& path : it->second.meshPaths)
        if (m_assets.queryMesh(path.c_str()) == 0)
            return false;
    return true;
}

bool SceneService::sceneLoadFailed(const char* cookedPath) const {
    if (!cookedPath) return false;
    std::string absPath = resolvePath(cookedPath);
    auto it = m_preloads.find(absPath);
    if (it == m_preloads.end()) return false;

    for (const auto& path : it->second.meshPaths)
        if (m_assets.loadFailed(path.c_str()))
            return true;
    return false;
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
