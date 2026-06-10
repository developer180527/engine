#pragma once
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>
#include <assetlib/asset_registry.h>

// ── AssetRef — the one way scenes reference assets on disk ─────────────────
//
//   { "asset": "<uuid>", "path": "assets/models/mask.glb" }
//
// `asset` is the identity: the assetlib UUID, stable across sessions,
// machines, renames and moves (the registry re-matches moved files by
// content hash). `path` is the project-relative source path — the human-
// readable fallback for unregistered assets, and what keeps git diffs
// legible. Never an absolute path, never a session handle.
//
// Resolution order on load:
//   1. uuid  → registry record → projectRoot / record.sourcePath
//   2. path  → projectRoot / path
//   3. legacy absolute "sourcePath" (old scenes) → used as-is if it exists,
//      else re-rooted at the project's assets/ folder by suffix match.
struct AssetRef {
    std::string uuid;   // "" = not registered in the asset DB
    std::string path;   // project-relative, POSIX separators ("" = none)

    bool empty() const { return uuid.empty() && path.empty(); }
};

namespace assetref {

// Build a ref from a runtime source path (absolute or engine:// scheme).
inline AssetRef make(const std::string& sourcePath,
                     const std::filesystem::path& projectRoot,
                     assetlib::AssetRegistry* lib) {
    AssetRef ref;
    if (sourcePath.empty()) return ref;

    // Virtual schemes (engine://primitive/...) pass through untouched.
    if (sourcePath.rfind("engine://", 0) == 0) {
        ref.path = sourcePath;
        return ref;
    }

    std::string rel = sourcePath;
    if (!projectRoot.empty()) {
        std::error_code ec;
        auto r = std::filesystem::relative(sourcePath, projectRoot, ec);
        // Keep the relative form only when the asset lives inside the project
        if (!ec && !r.empty() && r.generic_string().rfind("..", 0) != 0)
            rel = r.generic_string();
    }
    ref.path = rel;

    if (lib) {
        if (auto rec = lib->findBySourcePath(ref.path))
            ref.uuid = rec->uuid.toString();
    }
    return ref;
}

inline void toJson(const AssetRef& ref, nlohmann::json& j) {
    if (!ref.uuid.empty()) j["asset"] = ref.uuid;
    if (!ref.path.empty()) j["path"]  = ref.path;
}

// Reads the new keys, falling back to the legacy absolute "sourcePath".
inline AssetRef fromJson(const nlohmann::json& j) {
    AssetRef ref;
    ref.uuid = j.value("asset", std::string{});
    ref.path = j.value("path",  std::string{});
    if (ref.path.empty())
        ref.path = j.value("sourcePath", std::string{}); // legacy (absolute)
    return ref;
}

// Resolve to a loadable source path. Returns "" when nothing resolves.
inline std::string resolve(const AssetRef& ref,
                           const std::filesystem::path& projectRoot,
                           assetlib::AssetRegistry* lib) {
    namespace fs = std::filesystem;
    if (ref.empty()) return {};

    // Virtual schemes resolve as-is.
    if (ref.path.rfind("engine://", 0) == 0) return ref.path;

    // 1. UUID — survives renames/moves: the registry knows the current path.
    if (!ref.uuid.empty() && lib) {
        auto rec = lib->findByUUID(assetlib::UUID::fromString(ref.uuid));
        if (rec && !rec->sourcePath.empty()) {
            auto p = projectRoot / rec->sourcePath;
            if (fs::exists(p)) return p.string();
        }
    }

    // 2. Project-relative path.
    fs::path p = ref.path;
    if (p.is_relative() && !projectRoot.empty()) {
        auto abs = projectRoot / p;
        if (fs::exists(abs)) return abs.string();
        return {};
    }

    // 3. Legacy absolute path (old scene files).
    if (fs::exists(p)) return p.string();

    // 3b. The absolute path is from another machine/layout — re-root the
    // "assets/..." suffix onto this project.
    if (!projectRoot.empty()) {
        auto s = p.generic_string();
        auto pos = s.find("/assets/");
        if (pos != std::string::npos) {
            auto rebased = projectRoot / s.substr(pos + 1);
            if (fs::exists(rebased)) return rebased.string();
        }
    }
    return {};
}

} // namespace assetref
