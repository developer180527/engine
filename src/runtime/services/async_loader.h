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
#include <optional>

// Staging allocations cross this boundary by pointer only. They used to be
// `const bgfx::Memory*` behind a forward declaration, which kept the bgfx API
// out of consumers (audit A.4) but still named a backend in the runtime's
// header — so the type, not just the include, was the coupling. gpu::Blob is
// opaque by construction (render/gpu.h).

#include "render/gpu.h"
#include "assets/asset_storage.h"
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include <assetlib/asset_registry.h>
#include "assets/cook_requests.h"

// -----------------------------------------------------------------------
// GPU-ready data — ALL heavy work (parse + decode + the staging memcpy) is
// done on the worker thread. The main thread only creates handles, which is
// submitting a command — O(microseconds), no memcpy, no stall.
// -----------------------------------------------------------------------

struct SubRange { uint32_t indexOffset; uint32_t indexCount; uint32_t matIndex; };

struct MeshGPUData {
    gpu::Blob* vertexMem  = nullptr;          // pre-staged by worker
    gpu::Blob* indexMem   = nullptr;          // pre-staged by worker
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
    gpu::Blob* mem = nullptr;          // nullptr = no texture
    uint16_t w = 0, h = 0;
    uint32_t format = 0;               // assetlib::TextureFormatId (0=RGBA8)
    uint32_t mips   = 1;               // >1 = pre-mipped BC payload
    // How the GPU interprets the bytes. Linear by default so an unset field is
    // the pre-stage-A behaviour, never a silent sRGB decode of a normal map.
    gpu::ColourSpace cs = gpu::ColourSpace::Linear;
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

// Fully prepared asset: all CPU work AND all staging copies done on worker.
// drainOne() on the main thread just creates handles and spawns the entity.
struct LoadedAsset {
    // Loaded: staged and ready to upload. NotCooked: the mesh, or a texture
    // its materials name, has no Ready cook yet (`needsCook` says which).
    // Failed: final (the cook failed, or the cooked file is unusable).
    enum class Outcome : uint8_t { Loaded, NotCooked, Failed };
    std::string                   path;
    std::string                   name;
    Outcome                       outcome = Outcome::Failed;
    bool                          success = false;   // outcome == Loaded
    std::string                   error;
    std::vector<std::string>      needsCook;         // source paths
    std::vector<MeshGPUData>      meshes;
    std::vector<MaterialGPUData>  materials;

    // Animation data (extracted on worker — pure CPU, no GPU calls).
    bool                          hasSkeleton = false;
    Skeleton                      skeleton;
    std::vector<AnimClip>         animClips;
};

// Result passed to async-load callbacks so callers can wire animation components.
struct AsyncLoadResult {
    MeshHandle                  mesh;         // invalid: Failed, see `error`
    SkeletonHandle              skeleton;     // invalid if not skinned
    std::vector<AnimClipHandle> clips;        // empty if no animations
    std::string                 error;        // why, when `mesh` is invalid
};

// Where a requested source asset stands. `None`: never requested.
enum class AssetLoadState : uint8_t { None, Pending, Ready, Failed };

using OnLoaded = std::function<void(const AsyncLoadResult&, const std::string&)>;

// ── AsyncLoader — load an asset by its SOURCE path, from its COOKED version ──
// The editor asks for "assets/models/house.glb"; this finds the registry's
// Ready cook of it and loads that. COOKED ONLY (WO-018): nothing here parses a
// source format. Asking for an uncooked asset is a JOB:
//
//   * with a cooker (setCookRequests, the editor's CookService): the asset is
//     Pending, a cook is requested, and the request is retried on the worker
//     until the registry says Ready (load it) or Failed (report it). A cooked
//     mesh whose materials name uncooked textures waits for those too.
//   * without one (none set, or a NullCookRequests): Failed at once, with
//     "not cooked: <path>", logged once per path.
//
// The callback runs on the main thread (drainOne) exactly once per load():
// with the asset, or with an invalid mesh and `error`. While Pending, callers
// show a placeholder (state() says which it is).
class AsyncLoader {
public:
     AsyncLoader();
    ~AsyncLoader();
    AsyncLoader(const AsyncLoader&)            = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    // Thread-safe. A path already in flight or waiting gets the callback too.
    void load(const std::string& path, const std::string& name, OnLoaded cb);
    // Who cooks what is missing. Null (the default) is the null provider.
    // Not owned; must outlive the loader's pending requests.
    void setCookRequests(ICookRequests* c) { m_cook = c; }
    AssetLoadState state(const std::string& path) const;
    // Wire the asset registry so processFile can check for cooked versions.
    void setRegistry(assetlib::AssetRegistry* r) { m_registry = r; }
    void setProjectRoot(const std::filesystem::path& root) { m_projectRoot = root; }

    // Main thread only. O(microseconds) — only creates GPU handles.
    // Returns true if an asset was processed.
    bool drainOne(AssetStorage& storage);

    bool isLoading(const std::string& path) const;
    bool isLoaded (const std::string& path) const;
    // A Ready cooked version of this source exists, so load() takes the cooked
    // path (skeleton and clips included) rather than parsing the source.
    bool hasCooked(const std::string& path) const { return !cookedPathFor(path).empty(); }
    // Remove from loaded cache — enables hot-reload by allowing re-queue.
    void unload(const std::string& path);
    int  pendingCount() const;

private:
    struct LoadRequest   { std::string path, name; OnLoaded cb;
                           bool waitForTextures = true; };
    struct UploadRequest { LoadedAsset asset;       OnLoaded cb;
                           bool waitForTextures = true; };

    std::filesystem::path cookedPathFor(const std::string& path) const;
    std::optional<assetlib::AssetRecord> recordFor(const std::string& path) const;
    void        armWorker();          // pop one pending request onto the pool
    void        dispatch(LoadRequest req);   // run one load job, then self-chain
    LoadedAsset processFile(const std::string& path, const std::string& name,
                            bool waitForTextures);
    // A NotCooked result, on the main thread: request the cooks and park it,
    // or fail it when nothing can cook. Parked requests are retried every
    // kRetryDrains calls of drainOne (through the worker, which is the only
    // thread that reads the registry).
    void park(UploadRequest& req);
    void retryParked();
    void finishFailed(UploadRequest& req);
    static constexpr int kRetryDrains = 30;   // ~0.5 s at 60 drains a second

    bool                    m_jobBusy = false;   // guarded by m_pendingMtx

    mutable std::mutex      m_pendingMtx;
    std::queue<LoadRequest> m_pending;
    std::set<std::string>   m_inFlight;
    // Callbacks queued while the same path was already in-flight.
    // Drained in drainOne() alongside the primary callback.
    std::unordered_map<std::string, std::vector<OnLoaded>> m_waiters;

    mutable std::mutex        m_readyMtx;
    std::queue<UploadRequest> m_ready;

    // NotCooked requests waiting for a cook. Main thread only (drainOne).
    struct Parked { LoadRequest req; int retries = 0; };
    std::vector<Parked>                 m_parked;
    int                                 m_drainsSinceRetry = 0;
    std::set<std::string>               m_requested;   // cooks already asked for
    std::set<std::string>               m_failed;      // Failed paths (logged once)
    ICookRequests*                      m_cook = nullptr;

    mutable std::mutex                                    m_loadedMtx;
    std::unordered_map<std::string, AsyncLoadResult>      m_loadedResults; // path → cached result
    assetlib::AssetRegistry*  m_registry    = nullptr;
    std::filesystem::path     m_projectRoot;
};
