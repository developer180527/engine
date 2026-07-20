#pragma once
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <algorithm>
#include <filesystem>
#include "mesh_importer.h"
#include "assets/asset_storage.h"

class ImporterRegistry {
public:
    void registerImporter(std::unique_ptr<MeshImporter> imp) {
        m_importers.push_back(std::move(imp));
    }

    MeshImporter* findImporter(std::string_view ext) const {
        for (const auto& i : m_importers)
            if (i->supports(ext)) return i.get();
        return nullptr;
    }

    bool supports(std::string_view ext) const {
        return findImporter(ext) != nullptr;
    }

    bool supportsFile(const std::filesystem::path& p) const {
        std::string ext = p.extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        return supports(ext);
    }

    MeshImportResult load(const std::string& path, AssetStorage& storage) const {
        std::string ext = std::filesystem::path(path).extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        auto* imp = findImporter(ext);
        if (!imp) return MeshImportResult::fail("No importer for '." + ext + "'");
        return imp->load(path, storage);
    }

    MeshImportResult loadCached(const std::string& path, AssetStorage& storage) {
        std::string canonical;
        try { canonical = std::filesystem::weakly_canonical(path).string(); }
        catch (...) { canonical = path; }

        auto it = m_cache.find(canonical);
        if (it != m_cache.end()) {
            std::printf("[Registry] Cache hit: %s\n", canonical.c_str());
            // Return the FULL cached result — caching only the MeshHandle
            // stripped the skeleton + clips on every hit, so a skinned asset
            // loaded twice lost its animation on the second load (importer
            // audit: "Animation Data Loss on Cache Hit").
            return it->second;
        }
        auto result = load(path, storage);
        if (result.success) m_cache[canonical] = result;
        return result;
    }

    bool isLoaded(const std::string& path) const {
        try {
            return m_cache.count(
                std::filesystem::weakly_canonical(path).string()) > 0;
        } catch (...) { return m_cache.count(path) > 0; }
    }

    std::vector<std::string> supportedExtensions() const {
        static const char* kProbe[] = {
            "gltf","glb","fbx","obj","dae","blend","3ds","ply","stl",nullptr};
        std::vector<std::string> out;
        for (int i = 0; kProbe[i]; ++i)
            if (supports(kProbe[i])) out.push_back(kProbe[i]);
        return out;
    }

private:
    std::vector<std::unique_ptr<MeshImporter>> m_importers;
    std::map<std::string, MeshImportResult>    m_cache;
};
