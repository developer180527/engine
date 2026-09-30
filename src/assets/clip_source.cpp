#include "assets/clip_source.h"
#include "assets/anim_from_scene.h"
#include "assets/import/frontend_assimp.h"
#include "assets/import/frontend_cgltf.h"
#include "core/logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace imp {

::AnimClip readSourceClip(const std::string& sourcePath, const ::Skeleton& target) {
    // One front end per format, as the mesh cooker chooses (mesh_cooker.cpp).
    std::string ext = std::filesystem::path(sourcePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const bool gltf = ext == ".gltf" || ext == ".glb";
    const ImportResult r = gltf ? CgltfFrontend().importScene(sourcePath, {})
                                : AssimpFrontend().importScene(sourcePath, {});
    if (!r) {
        LOG_ERROR("Anim", "clip load failed: %s (%s)", sourcePath.c_str(), r.error().message.c_str());
        return {};
    }
    const ImportedScene& s = r.scene();
    if (s.clips.empty()) {
        LOG_ERROR("Anim", "no animations in %s", sourcePath.c_str());
        return {};
    }
    // v1: one clip per file (the Mixamo layout). Multi-take files can grow a
    // take name parameter later without changing callers.
    const Clip& c = s.clips.front();
    ::AnimClip clip = buildOzzClip(c, target);
    if (clip.mappedTracks < clip.totalTracks) {
        int shown = 0;
        for (const Track& t : c.tracks)
            if (target.findBone(t.bone) < 0 && shown++ < 6)
                LOG_WARN("Anim", "  unmapped track: %s", t.bone.c_str());
    }
    return clip;
}

}  // namespace imp
