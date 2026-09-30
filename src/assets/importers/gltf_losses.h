#pragma once
// ── gltf_losses — what the RUNTIME glTF importer does not apply ───────────────
//
// History: WO-002 used this to refuse cooking a skinned glTF, because neither
// glTF path read skins. Since WO-014 the COOK reads skins and clips (the cgltf
// import front end, src/assets/import/frontend_cgltf.cpp), so a cooked skinned
// glTF animates. What remains here is the editor's RUNTIME importer
// (GltfImporter), the uncooked-preview path: it still reads meshes only, and it
// says once per file what it leaves out until the asset is cooked. WO-018
// deletes that path, and this header with it.
#include <cgltf.h>

#include <cstddef>
#include <string>

struct GltfLosses {
    std::size_t meshes = 0, skins = 0, animations = 0;

    bool dropsAnything() const { return skins > 0 || animations > 0; }

    // What the runtime importer leaves out, or "" when nothing.
    std::string describe() const {
        auto n = [](std::size_t v, const char* one, const char* many) {
            return std::to_string(v) + " " + (v == 1 ? one : many);
        };
        if (!dropsAnything()) return {};
        return "the uncooked preview shows static geometry only: " + n(skins, "skin", "skins") + " and " +
               n(animations, "animation", "animations") + " apply once the asset is cooked";
    }
};

inline GltfLosses gltfLosses(const cgltf_data& d) {
    return {d.meshes_count, d.skins_count, d.animations_count};
}
