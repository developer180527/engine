#pragma once
#include "types.h"
#include <filesystem>
#include <assetlib/asset_registry.h>

namespace ab {

inline ImU32 stateColor(assetlib::AssetState s) {
    switch(s) {
        case assetlib::AssetState::Ready:   return IM_COL32(80,210,80,255);
        case assetlib::AssetState::Stale:
        case assetlib::AssetState::Dirty:   return IM_COL32(230,190,40,255);
        case assetlib::AssetState::Failed:
        case assetlib::AssetState::Missing: return IM_COL32(220,60,60,255);
        default:                            return IM_COL32(110,110,110,255);
    }
}

inline const char* stateName(assetlib::AssetState s) {
    switch(s) {
        case assetlib::AssetState::Ready:      return "Ready";
        case assetlib::AssetState::Stale:      return "Stale";
        case assetlib::AssetState::Dirty:      return "Dirty";
        case assetlib::AssetState::Failed:     return "Failed";
        case assetlib::AssetState::Missing:    return "Missing";
        case assetlib::AssetState::Importing:  return "Importing";
        case assetlib::AssetState::Registered: return "Registered";
        default:                               return "Unknown";
    }
}

// Look up cook state for a file by its absolute path.
// Returns empty RegistryInfo if registry is null or file not found.
inline RegistryInfo queryRegistry(assetlib::AssetRegistry*       reg,
                                  const std::string&             fullPath,
                                  const std::filesystem::path&   projectRoot,
                                  const std::filesystem::path&   cacheRoot) {
    RegistryInfo info;
    if (!reg) return info;
    std::error_code ec;
    auto rel = std::filesystem::relative(fullPath, projectRoot, ec);
    if (ec) return info;
    auto rec = reg->findBySourcePath(rel.generic_string());
    if (!rec) return info;
    info.found       = true;
    info.uuid        = rec->uuid.toString();
    info.state       = rec->state;
    info.cookVersion = rec->cookVersion;
    info.cookedAt    = rec->cookedAt;
    info.cookedPath  = rec->cookedPath;
    if (!rec->cookedPath.empty()) {
        std::error_code ec2;
        info.cookedBytes = std::filesystem::file_size(
            cacheRoot / rec->cookedPath, ec2);
    }
    return info;
}

} // namespace ab
