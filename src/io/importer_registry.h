#pragma once

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <algorithm>
#include <filesystem>

#include "mesh_importer.h"

// ImporterRegistry: maps file extensions to the importers that handle them.
//
// The registry is the single decision point for "can we load this file?"
// and "which importer should handle it?" Every importer (GltfImporter,
// future AssimpImporter) registers here. The asset browser, drag-drop,
// and any other loading path go through the registry — they never talk
// to a specific importer directly.
//
// Load cache: the registry caches every successfully loaded mesh by its
// canonical file path. Loading the same file twice returns the cached
// handle without hitting disk again. This is a minimal AssetManager
// precursor; when a real AssetManager exists, the cache moves there.
//
// Extension convention: lowercase, no leading dot ("gltf", "glb", "fbx").

class ImporterRegistry {
public:
    // Register an importer. Importers are checked in registration order;
    // the first one that claims to support the extension wins.
    void registerImporter(std::unique_ptr<MeshImporter> importer) {
        m_importers.push_back(std::move(importer));
    }

    // Find the importer for a given extension. Returns nullptr if none.
    MeshImporter* findImporter(std::string_view extension) const {
        for (const auto& imp : m_importers) {
            if (imp->supports(extension)) return imp.get();
        }
        return nullptr;
    }

    bool supports(std::string_view extension) const {
        return findImporter(extension) != nullptr;
    }

    // Check whether a file path has a supported extension.
    bool supportsFile(const std::filesystem::path& path) const {
        std::string ext = path.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        for (auto& c : ext) c = char(std::tolower(c));
        return supports(ext);
    }

    // Load a file, dispatching to the appropriate importer.
    // Does NOT cache — use loadCached for editor/browser workflows.
    MeshImportResult load(const std::string& path,
                          AssetRegistry& assets) const {
        std::string ext = std::filesystem::path(path).extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        for (auto& c : ext) c = char(std::tolower(c));

        MeshImporter* importer = findImporter(ext);
        if (!importer) {
            return MeshImportResult::fail(
                "No importer registered for extension '." + ext + "' "
                "(file: " + path + ")");
        }
        return importer->load(path, assets);
    }

    // Load a file with caching. If the file was already loaded this session,
    // returns the cached handle without re-parsing. Shows a console message
    // when returning from cache so it's visible in the log.
    MeshImportResult loadCached(const std::string& path,
                                AssetRegistry& assets) {
        namespace fs = std::filesystem;
        std::string canonical;
        try {
            canonical = fs::weakly_canonical(path).string();
        } catch (...) {
            canonical = path; // fallback if path doesn't exist yet
        }

        auto it = m_cache.find(canonical);
        if (it != m_cache.end()) {
            std::printf("[Registry] Cache hit: %s\n", canonical.c_str());
            return MeshImportResult::ok(it->second);
        }

        auto result = load(path, assets);
        if (result.success) {
            m_cache[canonical] = result.mesh;
        }
        return result;
    }

    // Check if a path is already in the load cache.
    bool isLoaded(const std::string& path) const {
        namespace fs = std::filesystem;
        try {
            std::string canonical = fs::weakly_canonical(path).string();
            return m_cache.count(canonical) > 0;
        } catch (...) {
            return m_cache.count(path) > 0;
        }
    }

    // Collect all unique extensions supported by registered importers.
    // Used by the asset browser to filter the file list.
    std::vector<std::string> supportedExtensions() const {
        std::vector<std::string> result;
        // We ask each importer what it supports by probing common extensions.
        static const char* kProbe[] = {
            "gltf", "glb", "fbx", "obj", "dae", "blend",
            "3ds", "ply", "stl", "x", "ms3d", nullptr
        };
        for (int i = 0; kProbe[i]; ++i) {
            if (supports(kProbe[i])) {
                result.push_back(kProbe[i]);
            }
        }
        return result;
    }

    const std::vector<std::unique_ptr<MeshImporter>>& importers() const {
        return m_importers;
    }

private:
    std::vector<std::unique_ptr<MeshImporter>> m_importers;

    // canonical path → MeshHandle
    std::map<std::string, MeshHandle> m_cache;
};
