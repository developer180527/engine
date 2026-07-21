#pragma once
#include "uuid.h"
#include "asset_registry.h"
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

    bool         isStale(const AssetRecord& rec) const;
    bool         hasCookerFor(const std::string& ext) const;
private:
    std::string  computeHash(const std::filesystem::path& p) const;
    ICooker*     findCooker(const std::string& ext) const;

    AssetRegistry&              m_registry;
    std::filesystem::path       m_projectRoot;
    std::filesystem::path       m_cacheRoot;
    std::vector<std::unique_ptr<ICooker>> m_cookers;

    // THE staleness version — bump when ANY cooker's output changes so the
    // workspace re-cooks. (Per-cooker kVersion constants exist but do NOT
    // feed isStale(); this one does. 11: glTF/GLB cook + scale-invariant
    // normal matrices. 12: BC7/BC5 block compression + mip chains. 13: fast
    // squish default (BC1/BC3 color, BC5 normal); BC7 opt-in via COOK_TEX_HQ.)
    static constexpr uint32_t   kCurrentCookVersion = 13;
};

} // namespace assetlib
