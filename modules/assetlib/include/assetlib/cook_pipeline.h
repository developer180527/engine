#pragma once
#include "uuid.h"
#include "asset_registry.h"
#include "ddc.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace assetlib {

// ── Cook context ──────────────────────────────────────────────────────────────
// Passed to ICooker::cook(). Cooker reads from sourcePath, writes to outputPath,
// and records any assets it depends on via addDependency().
struct CookContext {
    UUID                     uuid;
    std::filesystem::path    sourcePath;
    std::filesystem::path    outputPath;
    std::function<void(const UUID&)> addDependency;
    // A cooker producing files BEYOND outputPath (the mesh cooker writes
    // sibling .ctex blobs for embedded textures) MUST report each one here,
    // or those files won't travel with the DDC record and a cache hit on
    // another machine materializes a mesh whose textures don't exist.
    std::function<void(const std::filesystem::path&)> addOutput;
};

struct CookResult {
    bool        success    = false;
    bool        skipped    = false; // cooker can't handle this type — not an error
    std::string error;
    // Populated by the pipeline after a successful cook:
    std::string cookedPath;
};

// ── Cooker interface ──────────────────────────────────────────────────────────
// Implement this for each source format. The pipeline calls cook() when it
// detects a source file is stale (hash changed or never cooked).
class ICooker {
public:
    virtual ~ICooker() = default;
    // Which source file extensions does this cooker handle? e.g. {".fbx", ".obj"}
    virtual std::vector<std::string> extensions() const = 0;
    virtual CookResult               cook(const CookContext& ctx) = 0;

    // ── DDC identity — these three, plus the source bytes, ARE the cache key.
    // Stable id, never reused across cooker kinds (it namespaces the key).
    virtual const char* id() const = 0;
    // Bump whenever cook() output changes for identical input: format bumps,
    // encoder swaps, bug fixes. Only THIS cooker's outputs re-cook — a
    // texture-cooker bump never invalidates a single cooked mesh.
    virtual uint32_t    version() const = 0;
    // Everything else that alters output for the same source bytes: env
    // quality knobs, per-asset flags derived from the path (a filename-based
    // normal-map heuristic changes the encode!). MUST be deterministic for a
    // given (environment, source path). Default: no extra settings.
    virtual std::string settingsFingerprint(const CookContext& ctx) const {
        (void)ctx; return {};
    }

    // Estimated peak heap footprint, in bytes, of cooking this one asset.
    // The scheduler admits work against a memory budget (not a fixed thread
    // count) so a burst of 8K textures or high-poly meshes serializes instead
    // of OOM-ing the machine — heavy tasks run few-at-a-time, cheap ones pack.
    // Default: a generous multiple of source size; a cooker that can cheaply
    // predict better (a texture peeking its header dimensions) should override.
    virtual size_t estimatePeakBytes(const CookContext& ctx) const {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(ctx.sourcePath, ec);
        return ec ? ((size_t)64 << 20) : (size_t)sz * 10 + ((size_t)16 << 20);
    }
};

// ── Cook pipeline ─────────────────────────────────────────────────────────────
class CookPipeline {
public:
    explicit CookPipeline(AssetRegistry& registry,
                          std::filesystem::path projectRoot,
                          std::filesystem::path cacheRoot);

    void registerCooker(std::unique_ptr<ICooker> cooker);

    // Cook a single asset (by UUID). No-op if already up to date.
    CookResult cookOne(const UUID& uuid);

    // Cook all stale assets in the registry.
    // Returns number of assets cooked.
    // Progress callback receives (cooked, total).
    int        cookAll(std::function<void(int,int)> progress = {});
    // Cook a fixed UUID set across all cores. Registry I/O stays on the caller
    // thread; only cook() runs on the pool. onResult(sourcePath, success) is
    // invoked serialized as each finishes; shouldContinue() (optional) stops
    // dispatching new work when false.
    int        cookMany(const std::vector<UUID>& uuids,
                        std::function<void(const std::string&, bool)> onResult = {},
                        std::function<bool()> shouldContinue = {});

    // Force re-cook regardless of hash.
    CookResult forceRecook(const UUID& uuid);

    // Content-addressed staleness: an asset is stale iff the DDC key computed
    // from its CURRENT inputs (source hash ⊕ cooker id/version ⊕ settings)
    // differs from the key of the last attempt, or its materialized output
    // vanished. No mtime comparison, no global cook version — a cooker bump
    // re-keys (and thus re-cooks) only that cooker's assets.
    bool         isStale(const AssetRecord& rec) const;
    bool         hasCookerFor(const std::string& ext) const;

    // The DDC key for this record's current inputs ("" when no cooker/hash).
    std::string  currentKey(const AssetRecord& rec, ICooker* cooker) const;

    DdcStore&       ddc()       { return m_ddc; }
    const DdcStore& ddc() const { return m_ddc; }

private:
    ICooker*     findCooker(const std::string& ext) const;
    // Shared body of cookOne/forceRecook. useFetch=false bypasses the DDC
    // read path (forceRecook must not re-fetch the very blob under suspicion).
    CookResult   cookInternal(const UUID& uuid, bool useFetch);
    // A cook RECORD is a manifest of member blobs (primary .cooked + any
    // extra outputs like the mesh cooker's sibling .ctex textures), each
    // content-addressed by its own hash. Store the whole set / materialize
    // the whole set — all-or-nothing per key.
    bool         ddcStoreOutputs(const std::string& key,
                                 const std::filesystem::path& primary,
                                 const std::vector<std::filesystem::path>& extras);
    bool         ddcFetchOutputs(const std::string& key,
                                 const std::filesystem::path& outPath);
    // rec.sourceHash when it's a valid BLAKE3 hash (scan keeps it fresh);
    // hashes the file directly as a fallback (legacy/blank records).
    std::string  sourceHashFor(const AssetRecord& rec) const;
    void         commitResult(const UUID& uuid, const CookResult& res,
                              const std::string& key, uint32_t cookerVersion,
                              const std::filesystem::path& outPath,
                              const std::vector<UUID>& deps);

    AssetRegistry&              m_registry;
    std::filesystem::path       m_projectRoot;
    std::filesystem::path       m_cacheRoot;
    DdcStore                    m_ddc;      // roots from env (ENGINE_DDC[_SHARED])
    std::vector<std::unique_ptr<ICooker>> m_cookers;
};

} // namespace assetlib
