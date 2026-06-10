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
