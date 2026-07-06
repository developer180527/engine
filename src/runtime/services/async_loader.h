#pragma once
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <set>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

#include <bgfx/bgfx.h>
#include "assets/asset_storage.h"
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include <assetlib/asset_registry.h>

// -----------------------------------------------------------------------
// GPU-ready data — ALL heavy work (parse + decode + bgfx::copy memcpy)
// done on the worker thread. Main thread only creates bgfx handles, which
// is submitting a command — O(microseconds), no memcpy, no stall.
// -----------------------------------------------------------------------

struct SubRange { uint32_t indexOffset; uint32_t indexCount; uint32_t matIndex; };

struct MeshGPUData {
    const bgfx::Memory* vertexMem  = nullptr; // pre-copied by worker
    const bgfx::Memory* indexMem   = nullptr; // pre-copied by worker
    uint32_t  indexCount  = 0;
    bool      use32       = false;
    bool      doubleSided = false;
    bool      hasBounds   = false;
    float     boundsMin[3]{};
    float     boundsMax[3]{};
    uint32_t  matIndex    = 0;
    std::vector<SubRange> subRanges; // non-empty → multi-submesh
    bool      skinned     = false;  // use SkinnedVertex layout in drainOne
};

struct TextureGPUData {
    const bgfx::Memory* mem = nullptr; // nullptr = no texture
    uint16_t w = 0, h = 0;
};

struct MaterialGPUData {
    float          baseColorFactor[4] = {1, 1, 1, 1};
    float          roughness          = 0.7f;
    float          metallic           = 0.0f;
    TextureGPUData baseColorTexture;
    TextureGPUData normalMapTexture;
    std::string    baseColorName;
    std::string    normalMapName;
};

// Fully prepared asset: all CPU work AND all bgfx::copy() done on worker.
// drainOne() on the main thread just creates handles and spawns the entity.
struct LoadedAsset {
    std::string                   path;
    std::string                   name;
    bool                          success = false;
    std::string                   error;
    std::vector<MeshGPUData>      meshes;
    std::vector<MaterialGPUData>  materials;

    // Animation data (extracted on worker — pure CPU, no bgfx calls).
    bool                          hasSkeleton = false;
    Skeleton                      skeleton;
    std::vector<AnimClip>         animClips;
};

// Result passed to async-load callbacks so callers can wire animation components.
struct AsyncLoadResult {
    MeshHandle                  mesh;
    SkeletonHandle              skeleton;     // invalid if not skinned
    std::vector<AnimClipHandle> clips;        // empty if no animations
};

using OnLoaded = std::function<void(const AsyncLoadResult&, const std::string&)>;

class AsyncLoader {
public:
     AsyncLoader();
    ~AsyncLoader();
    AsyncLoader(const AsyncLoader&)            = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    // Thread-safe. Silently ignores paths already in flight.
    void load(const std::string& path, const std::string& name, OnLoaded cb);
    // Wire the asset registry so processFile can check for cooked versions.
    void setRegistry(assetlib::AssetRegistry* r) { m_registry = r; }
    void setProjectRoot(const std::filesystem::path& root) { m_projectRoot = root; }

    // Main thread only. O(microseconds) — only creates bgfx handles.
    // Returns true if an asset was processed.
    bool drainOne(AssetStorage& storage);

    bool isLoading(const std::string& path) const;
    bool isLoaded (const std::string& path) const;
    // Remove from loaded cache — enables hot-reload by allowing re-queue.
    void unload(const std::string& path);
    int  pendingCount() const;

private:
    struct LoadRequest   { std::string path, name; OnLoaded cb; };
    struct UploadRequest { LoadedAsset asset;       OnLoaded cb; };

    void        armWorker();          // pop one pending request onto the pool
    void        dispatch(LoadRequest req);   // run one load job, then self-chain
    LoadedAsset processFile(const std::string& path, const std::string& name);

    bool                    m_jobBusy = false;   // guarded by m_pendingMtx

    mutable std::mutex      m_pendingMtx;
    std::queue<LoadRequest> m_pending;
    std::set<std::string>   m_inFlight;
    // Callbacks queued while the same path was already in-flight.
    // Drained in drainOne() alongside the primary callback.
    std::unordered_map<std::string, std::vector<OnLoaded>> m_waiters;

    mutable std::mutex        m_readyMtx;
    std::queue<UploadRequest> m_ready;

    mutable std::mutex                                    m_loadedMtx;
    std::unordered_map<std::string, AsyncLoadResult>      m_loadedResults; // path → cached result
    assetlib::AssetRegistry*  m_registry    = nullptr;
    std::filesystem::path     m_projectRoot;
};
