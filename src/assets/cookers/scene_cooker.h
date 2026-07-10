#pragma once
#include <filesystem>

namespace assetlib { class AssetRegistry; }

// Standalone scene cooker — converts a JSON .scene file into a binary .cooked
// file with zero-parse-overhead entity records + string table.
//
// Minimal dependency surface: only needs JSON, assetlib types, and filesystem.
// CookService and EditorApp both call through here.
//
// Returns true on success. The assetLib pointer is used to resolve source paths
// to cooked paths (optional; pass nullptr if unavailable — entities will lack
// cookedPath entries in the binary, falling back to source import at runtime).
bool cookSceneFile(const std::filesystem::path& jsonPath,
                   const std::filesystem::path& outPath,
                   assetlib::AssetRegistry* assetLib = nullptr,
                   const std::filesystem::path& projectRoot = {});

// True if any asset THIS scene references has a cooked output newer than
// `cookedTime` (the scene binary's mtime). The per-scene dependency check
// that replaces the global assetsChanged flag — that flag re-cooked EVERY
// scene in the workspace whenever ANY single asset changed (cooker audit:
// "Global assetsChanged Scene Invalidation Loop"). Parses the scene JSON
// lightly (meshRenderer refs only); returns false on parse failure (a
// broken scene must not force workspace-wide recooks).
bool sceneDependsOnNewerAssets(const std::filesystem::path& jsonPath,
                               std::filesystem::file_time_type cookedTime,
                               assetlib::AssetRegistry* assetLib,
                               const std::filesystem::path& projectRoot,
                               const std::filesystem::path& cacheRoot);
