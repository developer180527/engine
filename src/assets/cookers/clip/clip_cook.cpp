#include "assets/cookers/clip/clip_cook.h"

#include "animation/cooked_clip.h"
#include "assets/anim_from_scene.h"
#include "assets/import/imported_scene_check.h"
#include "core/logger.h"

#include <fstream>

namespace clipcook {

assetlib::CookResult cookClip(const imp::ImportedScene& s, const assetlib::CookContext& ctx) {
    const std::string label = ctx.sourcePath.filename().string();
    auto refuse = [](std::string why) { return assetlib::CookResult{.success = false, .error = std::move(why)}; };

    if (const auto bad = imp::checkScene(s); !bad.empty())
        return refuse(std::string("invalid import (") + bad[0].check + "): " + bad[0].detail);
    for (const imp::Dropped& d : s.dropped)
        if (d.effect == imp::Dropped::Effect::Wrong)
            return refuse(std::string("cannot cook without the ") + imp::toString(d.kind) + ": " + d.what);
    if (s.clips.empty()) return refuse("no clip to cook");

    anim::CookedClip c;
    imp::toRawClip(s.clips.front(), c.trackBones, c.keys);
    if (!c.keys.Validate()) return refuse("clip '" + s.clips.front().name + "' has invalid keys");
    const std::vector<uint8_t> bytes = anim::encodeCookedClip(c);

    std::ofstream f(ctx.outputPath, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()))
        return refuse("cannot write " + ctx.outputPath.string());

    for (size_t i = 1; i < s.clips.size(); ++i)
        LOG_WARN("ClipCooker", "%s: clip '%s' not cooked: one clip per file (the first, '%s')",
                 label.c_str(), s.clips[i].name.c_str(), s.clips.front().name.c_str());
    LOG_INFO("ClipCooker", "%s -> clip '%s' (%.2fs, %zu tracks, %zu bytes)", label.c_str(),
             s.clips.front().name.c_str(), s.clips.front().duration, c.trackBones.size(), bytes.size());
    return {.success = true};
}

}  // namespace clipcook
