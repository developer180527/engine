#pragma once
#include "uuid.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The COOKER CONTRACT — everything needed to implement or invoke a cooker,
// with no dependency on the pipeline that drives them. Cooker implementations
// and the out-of-process worker include this; only the orchestrator needs
// <assetlib/cook_pipeline.h>.
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

} // namespace assetlib
