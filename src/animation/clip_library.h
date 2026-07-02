#pragma once
// ── ClipLibrary — standalone animation clips as assets ───────────────────────
// Loads an animation clip from its OWN source file (a Mixamo clip FBX:
// skeleton + animation tracks, no mesh) and BINDS it to a target skeleton:
// track bone-names resolve through Skeleton::findBone, baking boneIndex into
// each channel — after binding, sampling is pure index math, zero per-frame
// string lookups. Unmapped tracks are skipped with a warning (that's the
// name-based binding contract; retargeting across different rigs is a future
// upgrade of exactly this step).
//
// A clip bound to two different skeletons is two registry entries — the cache
// key is (source path | skeleton handle). Loads are synchronous (clip files
// are small); an async path can come with the animation cooker.
#include "animation/animation_clip.h"
#include "animation/clip_registry.h"
#include "animation/skeleton.h"
#include "animation/ozz_bridge.h"               // buildOzzClip (name binding)
#include "core/handle.h"
#include "core/logger.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <filesystem>
#include <string>
#include <unordered_map>

class ClipLibrary {
public:
    // Load (or return cached) `sourcePath` bound to `skeleton`. Uses the same
    // Assimp flags as the mesh importer so node transforms decompose
    // identically to the skeleton they animate.
    AnimClipHandle load(const std::string& sourcePath,
                        SkeletonHandle skelHandle, const Skeleton& skeleton,
                        AnimClipRegistry& clips) {
        const std::string key = sourcePath + "|" + std::to_string(skelHandle.id);
        if (auto it = m_cache.find(key); it != m_cache.end()) return it->second;

        Assimp::Importer imp;
        // MUST match the skinned-mesh import (async_loader.cpp): pivots baked,
        // not split into $AssimpFbx$ helper nodes — otherwise rotation tracks
        // land on synthetic node names that don't exist on the skeleton.
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* scene = imp.ReadFile(sourcePath,
            aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
            aiProcess_JoinIdenticalVertices | aiProcess_SortByPType);
        if (!scene || !scene->mRootNode) {
            LOG_ERROR("Anim", "clip load failed: %s (%s)", sourcePath.c_str(),
                      imp.GetErrorString());
            return {};
        }
        if (scene->mNumAnimations == 0) {
            LOG_ERROR("Anim", "no animations in %s", sourcePath.c_str());
            return {};
        }
        if (!skeleton.ozz) {
            LOG_ERROR("Anim", "target skeleton has no ozz runtime data");
            return {};
        }

        // v1: one clip per file (the Mixamo layout). Multi-take files can grow
        // a takeName parameter later without changing callers.
        const aiAnimation* src = scene->mAnimations[0];
        AnimClip clip = anim::buildOzzClip(src, skeleton,
            std::filesystem::path(sourcePath).stem().string());
        if (clip.mappedTracks == 0) {
            LOG_ERROR("Anim", "'%s': no tracks match the target skeleton "
                      "(%d tracks, %d bones) — wrong rig?",
                      clip.name.c_str(), clip.totalTracks, skeleton.boneCount());
            return {};
        }
        if (!clip.valid()) return {};   // builder failure (already logged)
        if (clip.mappedTracks < clip.totalTracks) {
            LOG_WARN("Anim", "'%s': %d/%d tracks unmapped on this skeleton",
                     clip.name.c_str(), clip.totalTracks - clip.mappedTracks,
                     clip.totalTracks);
            int shown = 0;
            for (unsigned c = 0; c < src->mNumChannels && shown < 6; ++c) {
                const char* n = src->mChannels[c]->mNodeName.C_Str();
                if (skeleton.findBone(n) < 0) {
                    LOG_WARN("Anim", "  unmapped track: %s", n);
                    ++shown;
                }
            }
        }

        AnimClipHandle h = clips.add(std::move(clip));
        m_cache[key] = h;
        LOG_SUCCESS("Anim", "bound clip '%s' (%.2fs, %d/%d tracks) from %s",
                    clips.get(h)->name.c_str(), clips.get(h)->duration,
                    clips.get(h)->mappedTracks, clips.get(h)->totalTracks,
                    std::filesystem::path(sourcePath).filename().string().c_str());
        return h;
    }

    // Registries reset (project switch / registry clear) — drop stale handles.
    void clear() { m_cache.clear(); }

private:
    std::unordered_map<std::string, AnimClipHandle> m_cache; // "path|skelId"
};
