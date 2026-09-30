#pragma once
// Asset-browser directory scanning: what one folder of the project contains,
// with each file's importer support and cook state. No GUI — shared by every
// front end (the ImGui panel today; libgui experiments).
#include "types.h"
#include "registry.h"
#include <functional>
#include <assetlib/asset_registry.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace ab {

// OS file-manager junk that should never show up as "assets":
// macOS (.DS_Store, AppleDouble ._*, .localized, Spotlight/Trash/fsevents),
// Windows (Thumbs.db, desktop.ini, ehthumbs.db), Linux/KDE (.directory).
inline bool isOsJunk(const std::string& name) {
    if (name == ".DS_Store" || name == ".localized" ||
        name == ".Spotlight-V100" || name == ".Trashes" || name == ".fseventsd")
        return true;
    if (name.rfind("._", 0) == 0) return true;              // AppleDouble sidecars
    if (name == "Thumbs.db" || name == "ehthumbs.db" || name == "desktop.ini")
        return true;
    if (name == ".directory") return true;                   // KDE folder metadata
    return false;
}

// Whether a file (by lower-case extension, with the dot) is a model the
// editor can spawn: one the cook stack turns into a mesh. The editor answers
// from MeshCooker's own list; empty means nothing is spawnable.
using SpawnableFn = std::function<bool(const std::string& ext)>;

// Scan one directory level and return sorted FileEntry list.
inline std::vector<FileEntry> scanDir(const std::filesystem::path& dir,
                                      const SpawnableFn&           spawnable,
                                      assetlib::AssetRegistry*     reg,
                                      const std::filesystem::path& projectRoot,
                                      const std::filesystem::path& cacheRoot) {
    std::vector<FileEntry> out;
    if (!std::filesystem::is_directory(dir)) return out;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        if (isOsJunk(de.path().filename().string())) continue;
        FileEntry e;
        e.name     = de.path().filename().string();
        e.fullPath = de.path().string();
        e.isDir    = de.is_directory();
        if (!e.isDir) {
            if (!de.is_regular_file()) continue;
            e.ext       = lowerExt(de.path());
            e.supported = spawnable && spawnable(e.ext);
            try { e.sizeBytes = de.file_size(); } catch (...) {}
            e.reg = queryRegistry(reg, e.fullPath, projectRoot, cacheRoot);
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b){
        if (a.isDir    != b.isDir)    return a.isDir    > b.isDir;
        if (a.supported!= b.supported)return a.supported> b.supported;
        return a.name < b.name;
    });
    return out;
}

// A folder's subdirectories, sorted, OS junk skipped. Empty on any error.
inline std::vector<std::filesystem::path> listSubdirs(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> subs;
    try {
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.is_directory() && !isOsJunk(e.path().filename().string()))
                subs.push_back(e.path());
    } catch (...) {}
    std::sort(subs.begin(), subs.end());
    return subs;
}

} // namespace ab
