// clip_binding_test — headless proof of standalone-clip name binding.
//
//   usage: clip_binding_test <character.fbx> <clip.fbx>
//
// Extracts the skeleton from the CHARACTER file, then loads the CLIP from its
// own separate file via ClipLibrary — the Mixamo layout. Passing means the
// clip's bone tracks resolved onto the character's skeleton by name (baked
// boneIndex channels), the duration is sane, and the cache returns the same
// handle on a second load. Pure CPU: no window, no bgfx.
#include "animation/assimp_skeleton_loader.h"
#include "animation/clip_library.h"
#include "animation/clip_registry.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: clip_binding_test <character.fbx> <clip.fbx>\n");
        return 2;
    }

    // Skeleton from the character file (same settings as the runtime importer:
    // async_loader.cpp bakes FBX pivots instead of splitting helper nodes).
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices | aiProcess_SortByPType);
    if (!scene) { std::fprintf(stderr, "FAIL: character load: %s\n", imp.GetErrorString()); return 1; }

    Skeleton skel = anim::extractSkeleton(scene);
    if (skel.boneCount() == 0) { std::fprintf(stderr, "FAIL: no bones in character\n"); return 1; }
    std::printf("character skeleton: %d bones\n", skel.boneCount());

    // Standalone clip bound to that skeleton.
    AnimClipRegistry clips;
    ClipLibrary      lib;
    SkeletonHandle   sh{1};
    AnimClipHandle h = lib.load(argv[2], sh, skel, clips);
    if (!h.valid()) { std::fprintf(stderr, "FAIL: clip did not bind\n"); return 1; }

    const AnimClip* clip = clips.get(h);
    if (!clip || clip->channels.empty()) { std::fprintf(stderr, "FAIL: clip has no bound channels\n"); return 1; }
    if (clip->duration <= 0.0f)          { std::fprintf(stderr, "FAIL: zero duration\n"); return 1; }
    for (const auto& ch : clip->channels)
        if (ch.boneIndex < 0 || ch.boneIndex >= skel.boneCount()) {
            std::fprintf(stderr, "FAIL: channel with out-of-range boneIndex %d\n", ch.boneIndex);
            return 1;
        }

    // Cache: same (path, skeleton) must return the identical handle.
    AnimClipHandle h2 = lib.load(argv[2], sh, skel, clips);
    if (h2.id != h.id) { std::fprintf(stderr, "FAIL: cache miss on second load\n"); return 1; }

    std::printf("clip_binding_test: PASS — '%s' %.2fs, %zu channels bound to %d bones\n",
                clip->name.c_str(), clip->duration, clip->channels.size(),
                skel.boneCount());
    return 0;
}
