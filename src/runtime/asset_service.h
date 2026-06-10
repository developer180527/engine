#pragma once

#include "core/handle.h"
#include <filesystem>
#include <memory>

// Forward declarations — keeps include footprint small for consumers.
class AssetRegistry;
class TextureRegistry;
class MaterialRegistry;
namespace assetlib { class AssetRegistry; }

// ---------------------------------------------------------------------------
// AssetService — flat, FFI-friendly API for loading and unloading cooked
// assets at runtime. This is the scripting contract: every method uses only
// primitives, const char*, and typed handles so thin per-language wrappers
// (Lua, C#, etc.) can call through without marshaling structs.
//
// Owns no storage — delegates to the three runtime registries (meshes,
// textures, materials) that EngineRuntime owns. The service is infrastructure,
// not a plugin; it's available before any IEnginePlugin attaches.
//
// Sync load*() methods do file I/O + GPU handle creation on the calling
// thread — suitable for small/critical assets or preload phases.
//
// Async load*Async() methods queue the file I/O onto a background worker;
// call drainUploads() on the main thread each frame to finalize GPU handles.
// Scripts poll via query*() to get the handle once ready.
// ---------------------------------------------------------------------------
class AssetService {
public:
    struct Config {
        AssetRegistry&    meshes;
        TextureRegistry&  textures;
        MaterialRegistry& materials;
        assetlib::AssetRegistry* assetLib = nullptr;  // for texture path resolution
        std::filesystem::path    projectRoot;          // root of the game project
    };

    explicit AssetService(Config cfg);
    ~AssetService();   // stops worker thread

    AssetService(const AssetService&)            = delete;
    AssetService& operator=(const AssetService&) = delete;

    // ----- Sync Load -----

    // Load a cooked mesh binary (.cooked), including any embedded materials
    // and their referenced textures. Relative paths are resolved against
    // the project's .cache directory.
    // Returns a valid MeshHandle on success, invalid (id=0) on failure.
    MeshHandle    loadMesh(const char* cookedPath);

    // Load a cooked texture binary (.cooked). Relative paths are resolved
    // against the project's .cache directory.
    TextureHandle loadTexture(const char* cookedPath);

    // ----- Async Load -----
    // Queue a cooked asset for background loading. The worker thread parses
    // the file and prepares GPU data; call drainUploads() each frame on the
    // main thread to finalize bgfx handles. Duplicate requests are ignored.

    void loadMeshAsync(const char* cookedPath);
    void loadTextureAsync(const char* cookedPath);

    // ----- Async Query -----
    // Returns the handle (as uint32_t) if the asset has finished loading
    // and been drained, 0 if still pending or failed. Poll from scripts.

    uint32_t queryMesh(const char* cookedPath)    const;
    uint32_t queryTexture(const char* cookedPath) const;
    bool     isLoading(const char* cookedPath)    const;
    int      pendingCount()                       const;

    // ----- Drain -----
    // Main thread only — finalize ONE completed async load per call
    // (creates bgfx handles, registers in registries). Returns true if
    // an asset was processed. Call once per frame to keep frame time smooth,
    // or in a loop if you want to flush everything immediately.

    bool drainUploads();

    // ----- Unload -----

    bool unloadMesh(MeshHandle h);
    bool unloadTexture(TextureHandle h);
    bool unloadMaterial(MaterialHandle h);

    // ----- Query -----

    size_t meshCount()     const;
    size_t textureCount()  const;
    size_t materialCount() const;

    // ----- Late-binding configuration -----
    // Must be called before any async loads (typically from main.cpp at startup).

    void setAssetLib(assetlib::AssetRegistry* lib) { m_assetLib = lib; }

    void setProjectRoot(const std::filesystem::path& root) {
        m_projectRoot = root;
        m_cacheRoot   = root.empty() ? std::filesystem::path{}
                                     : root / ".cache";
    }

private:
    AssetRegistry&    m_meshes;
    TextureRegistry&  m_textures;
    MaterialRegistry& m_materials;

    assetlib::AssetRegistry*  m_assetLib;
    std::filesystem::path     m_projectRoot;
    std::filesystem::path     m_cacheRoot;    // m_projectRoot / ".cache"

    // Sync helpers
    TextureHandle loadTextureFromCooked(const std::filesystem::path& absPath);
    TextureHandle resolveTexture(const char* texPath,
                                 const std::filesystem::path& sourceDir);

    // Async internals (pimpl — keeps threading headers out of this header)
    struct AsyncState;
    std::unique_ptr<AsyncState> m_async;

    void        ensureWorker();
    void        workerLoop();
    std::string resolvePath(const char* cookedPath) const;
};
