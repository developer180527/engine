#pragma once

#include "core/handle.h"
// Header-only and deliberately bgfx-free, so including it here does not drag
// the graphics API into every consumer of AssetService.
#include "render/gpu_resource_cache.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_set>

// Forward declarations — keeps include footprint small for consumers.
class AssetRegistry;
class TextureRegistry;
class MaterialRegistry;
class SkeletonRegistry;
class AnimClipRegistry;
namespace assetlib { class AssetRegistry; }

#include <vector>
#include <unordered_map>
#include <unordered_set>

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
        // Optional — enables SKINNED cooked streaming: v3 cooked meshes carry
        // bones + ozz skeleton/clip archives, registered here on load. Null
        // = skinned cooked meshes are rejected (bare tools).
        SkeletonRegistry* skeletons = nullptr;
        AnimClipRegistry* clips     = nullptr;
    };

    // Skinned payload of a loaded cooked mesh — what a spawner needs to wire
    // SkinnedMesh + Animator on the entity (handles are session-local).
    struct MeshSkin {
        SkeletonHandle              skeleton;
        std::vector<AnimClipHandle> clips;
    };

    explicit AssetService(Config cfg);
    ~AssetService();   // stops worker thread

    AssetService(const AssetService&)            = delete;
    AssetService& operator=(const AssetService&) = delete;

    // ----- Sync Load -----

    // Load a cooked mesh binary (.cooked), including any embedded materials
    // and their referenced textures. Relative paths are resolved against
    // the project's .cache directory. Skinned v3 meshes (bones + ozz
    // archives) register their skeleton/clips when Config registries are
    // wired; outSkin (optional) receives the handles for component wiring.
    // Returns a valid MeshHandle on success, invalid (id=0) on failure.
    MeshHandle    loadMesh(const char* cookedPath, MeshSkin* outSkin = nullptr);

    // Load a cooked texture binary (.cooked). Relative paths are resolved
    // against the project's .cache directory.
    TextureHandle loadTexture(const char* cookedPath);

    // Load a cooked MATERIAL **by its authored name** ("zombie_sickly").
    //
    // By name, not by cooked path: a cooked file is <uuid>.cooked, and no one
    // should hand-write a uuid to reference their own material. The name is the
    // addressable identity — the same rule shaders follow, and for the same
    // reason (a shipped dist has no registry to map paths to uuids).
    //
    // Repeat calls return the SAME handle: a spawner asking per entity must not
    // allocate a Material per spawn.
    //
    // Invalid handle on failure, having logged why. A material that will not
    // load must NOT silently fall back to the fixed struct — that renders
    // something plausible and hides the problem.
    MaterialHandle loadMaterialAsset(const char* name);

    // Every cooked material name available, sorted. For the editor's material
    // picker, and for telling an author what they could have typed.
    std::vector<std::string> materialNames();

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
    // True if the last async load of this path FAILED (parse error, missing
    // file, GPU buffer failure). Distinguishes "will never load" from "still
    // loading" — without it a failed asset is indistinguishable from a
    // never-requested one (audit H.6). A new load*Async() clears it (retry).
    bool     loadFailed(const char* cookedPath)   const;
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

    // Read-only view of the texture dedup cache, for the VRAM census
    // (render/diag/resource_census.h). The census tooling was built in Phase 2
    // and could not be USED, because the cache it reports on was private with no
    // accessor — a diagnostic nothing can reach is not a diagnostic.
    const gpucache::GpuResourceCache<TextureHandle>& textureCache() const {
        return m_texCache;
    }

    // ----- Residency (audit Q6: the loaded-mesh cache grew without bound) --
    // Byte budget for async-loaded cooked meshes. 0 = unbounded (default —
    // the editor keeps everything). Streaming eviction only makes sense
    // with a budget set (players, big worlds).
    void     setResidencyBudget(uint64_t bytes);
    uint64_t residentBytes() const;

    // Evict least-recently-USED loaded meshes until under budget, skipping
    // ids in `inUse` — assets a live MeshRenderer still references must
    // never vanish under the renderer. The runtime calls this periodically
    // with a scan of both worlds; queryMesh()/loadMeshAsync() stamp use.
    // Evicted paths reload transparently on the next loadMeshAsync (the
    // cooked file is still on disk — this frees GPU + registry residency,
    // not the asset). Returns the number of meshes evicted.
    size_t evictOverBudget(const std::unordered_set<uint32_t>& inUse);

private:
    AssetRegistry&    m_meshes;
    TextureRegistry&  m_textures;
    MaterialRegistry& m_materials;
    SkeletonRegistry* m_skeletons = nullptr;   // optional (skinned streaming)
    AnimClipRegistry* m_clips     = nullptr;

    assetlib::AssetRegistry*  m_assetLib;
    std::filesystem::path     m_projectRoot;
    std::filesystem::path     m_cacheRoot;    // m_projectRoot / ".cache"
    uint64_t                  m_residencyBudget = 0;   // bytes; 0 = unbounded

    // Texture identity + refcounts (renderer audit R1). Before this, every
    // load of the same cooked texture uploaded ANOTHER copy to the GPU —
    // loadTextureFromCooked had no dedup whatsoever, and two materials naming
    // the same image paid for it twice. Keyed by cooked path; the cache owns
    // the mapping, TextureRegistry still owns the resource.
    // See docs/plans/renderer-audit-and-plan.md Phase 1.
    gpucache::GpuResourceCache<TextureHandle> m_texCache;

    // ── Loaded-mesh cache: path -> handle, shared by the SYNC and ASYNC paths
    // It lives here, not inside AsyncState, because it is a resource cache and
    // not an async concern — and because putting it there made it invisible to
    // synchronous loads. AsyncState is created lazily by ensureWorker(), so a
    // scene that loads every mesh synchronously (the cooked fast path) never had
    // a cache at all: each of 50 000 entities created its own vertex+index
    // buffers and bgfx's 4 096-handle pool ran out after 4 089 of them.
    // Textures avoided this because m_texCache above already sat at this level.
    struct MeshResidency {
        MeshHandle h;
        uint64_t   bytes   = 0;
        uint64_t   lastUse = 0;
    };
    mutable std::mutex                             m_loadedMtx;
    // `mutable` alongside m_useClock: queryMesh() is const and must still
    // stamp LRU use, or the cache evicts what is being actively read.
    mutable std::unordered_map<std::string, MeshResidency> m_loadedMeshes;
    mutable uint64_t                               m_useClock = 0;   // LRU ticks
    // `mutable` because queryMesh() is const and a query IS a use: not
    // stamping it would make the LRU evict assets that are actively read.

    // Sync helpers
    TextureHandle loadTextureFromCooked(const std::filesystem::path& absPath);
    TextureHandle resolveTexture(const char* texPath,
                                 const std::filesystem::path& sourceDir);

    // name -> cooked path, built once by scanning <cache>/materials. Mirrors
    // ShaderLibrary's index; see loadMaterialAsset for why it is by name.
    void buildMaterialIndex();
    std::unordered_map<std::string, std::filesystem::path> m_materialByName;
    std::unordered_map<std::string, MaterialHandle>        m_materialLoaded;
    // Reverse of m_materialLoaded, so unloadMaterial can drop the name entry
    // for a handle. Kept as a map rather than searched linearly because unload
    // is on the same per-entity path as load.
    std::unordered_map<uint32_t, std::string>             m_materialByHandle;
    // Names already reported missing — the error is loud and expensive to
    // build, and the lookup that produces it runs every call by design.
    std::unordered_set<std::string>                       m_materialMissWarned;
    bool m_materialIndexBuilt = false;

    // Async internals (pimpl — keeps threading headers out of this header)
    struct AsyncState;
    std::unique_ptr<AsyncState> m_async;

    void        ensureWorker();
    void        workerLoop();
    std::string resolvePath(const char* cookedPath) const;
};
