#pragma once

#include "core/handle.h"
#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>
#include <unordered_map>

// Forward declarations
class AssetService;
class AssetRegistry;
class TextureRegistry;
class MaterialRegistry;
class PrimitiveLibrary;
namespace flecs { struct world; }

// ═══════════════════════════════════════════════════════════════════════════
// SceneService — runtime scene loading from cooked binary .scene files.
//
// Built on top of AssetService (Phase 1). Reads the binary scene format
// (memcpy entity records + string table), resolves asset references via
// AssetService::loadMesh(), spawns entities into the ECS world.
//
// Supports:
//   - Sync loadScene()   → parse + load assets + spawn entities
//   - unloadScene()      → destroy entities + unload their assets
//   - preloadScene()     → async pre-load all assets, poll isSceneReady()
//
// This is engine infrastructure (like AssetService), not a plugin.
// All methods use primitives/const char* for FFI compatibility.
// ═══════════════════════════════════════════════════════════════════════════
class SceneService {
public:
    struct Config {
        AssetService&     assets;
        AssetRegistry&    meshes;
        TextureRegistry&  textures;
        MaterialRegistry& materials;
        flecs::world&     world;
        PrimitiveLibrary* primitives = nullptr;
    };

    explicit SceneService(Config cfg);
    ~SceneService();

    SceneService(const SceneService&)            = delete;
    SceneService& operator=(const SceneService&) = delete;

    // ── Sync scene load ──
    // Load a cooked binary scene: read entities, load all referenced meshes/
    // textures via AssetService, spawn entities into the ECS world.
    // Returns a scene handle (>0 on success, 0 on failure).
    // Relative paths resolved against .cache/ directory.
    uint32_t loadScene(const char* cookedPath);

    // ── Unload ──
    // Destroy all entities spawned by this scene and unload their assets.
    // Returns true if the scene was found and unloaded.
    bool unloadScene(uint32_t sceneHandle);

    // ── Async preload ──
    // Parse the scene file and kick off loadMeshAsync/loadTextureAsync for
    // every referenced asset. Does NOT spawn entities yet. Call isSceneReady()
    // to poll, then loadScene() once ready (assets will already be cached).
    void preloadScene(const char* cookedPath);

    // Returns true only when EVERY asset for the given scene is loaded.
    // A scene with a failed asset is never "ready" — poll sceneLoadFailed()
    // to distinguish "still loading" from "will never be ready" and bail.
    bool isSceneReady(const char* cookedPath) const;

    // True if any of the scene's preloaded assets failed to load (the scene
    // will never become ready without a retry). Audit H.6: failed assets
    // used to silently report as ready.
    bool sceneLoadFailed(const char* cookedPath) const;

    // ── Query ──
    uint32_t sceneEntityCount(uint32_t sceneHandle) const;
    uint32_t activeSceneCount() const;

    // ── Configuration ──
    void setCacheRoot(const std::filesystem::path& root) { m_cacheRoot = root; }

private:
    // A loaded scene tracks which entities and assets it owns for cleanup.
    struct LoadedScene {
        std::string                path;
        std::vector<uint64_t>      entityIds;      // flecs entity_t values
        std::vector<MeshHandle>    meshHandles;     // for unload
        std::vector<TextureHandle> textureHandles;  // TODO: track from mesh loads
        std::vector<MaterialHandle> materialHandles;
    };

    AssetService&     m_assets;
    AssetRegistry&    m_meshes;
    TextureRegistry&  m_textures;
    MaterialRegistry& m_materials;
    flecs::world&     m_world;
    PrimitiveLibrary* m_primitives = nullptr;
    std::filesystem::path m_cacheRoot;

    uint32_t m_nextHandle = 1;
    std::unordered_map<uint32_t, LoadedScene> m_scenes;

    // Preload tracking
    struct PreloadEntry {
        std::vector<std::string> meshPaths;
    };
    std::unordered_map<std::string, PreloadEntry> m_preloads;

    std::string resolvePath(const char* cookedPath) const;
};
