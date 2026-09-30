#pragma once
// ── gltf_losses — what in a glTF the engine cannot carry yet (WO-002) ─────────
//
// Neither glTF path reads skins or animations: `cookGltf` (the cooker) and
// `GltfImporter` (the editor's runtime importer) both walk meshes and nothing
// else, and Assimp is built without glTF, so nothing falls back. A skinned
// `.glb` therefore cooked as a STATIC mesh — skeleton and animations silently
// gone — and the cook reported success.
//
// One judgement, shared by both paths so they cannot disagree about it:
//
//   skins > 0                  REFUSE the cook. A character without its
//                              skeleton is not a degraded asset, it is a wrong
//                              one; success would be a lie.
//   no meshes, animations > 0  REFUSE the cook, naming what it is. It used to
//                              fail as "no triangle geometry", which sends you
//                              looking for a geometry bug.
//   meshes + node animations   COOK the static mesh and SAY the animations were
//   (no skin)                  dropped. The mesh alone is still correct — a door
//                              is still a door when it does not swing — and
//                              refusing would break props that cook today.
//
// Removed when WO-014 lands (glTF skins and animations supported for real);
// its tests invert the WO-002 ones.
#include <cgltf.h>

#include <cstddef>
#include <string>

struct GltfLosses {
    std::size_t meshes = 0, skins = 0, animations = 0;

    bool refuseCook() const { return skins > 0 || (meshes == 0 && animations > 0); }
    bool dropsAnything() const { return skins > 0 || animations > 0; }

    // Why the cook is refused, or what the static cook leaves out. Empty when
    // nothing is lost.
    std::string describe() const {
        auto n = [](std::size_t v, const char* one, const char* many) {
            return std::to_string(v) + " " + (v == 1 ? one : many);
        };
        if (skins > 0)
            return "skinned glTF is not supported yet: " + n(skins, "skin", "skins")
                 + " and " + n(animations, "animation", "animations")
                 + " would be lost (WO-014)";
        if (meshes == 0 && animations > 0)
            return "animation-only glTF: " + n(animations, "animation", "animations")
                 + ", and glTF clips are not supported yet (WO-014)";
        if (animations > 0)
            return n(animations, "node animation is", "node animations are")
                 + " not cooked; the static mesh is (WO-014)";
        return {};
    }
};

inline GltfLosses gltfLosses(const cgltf_data& d) {
    return {d.meshes_count, d.skins_count, d.animations_count};
}
