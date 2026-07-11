#pragma once
// ── ozz bridge — Assimp import data -> ozz runtime structures ────────────────
// The single seam between our Assimp-based import and the ozz animation
// backbone. Two builders:
//
//   buildOzzSkeleton(Skeleton&)   engine Skeleton -> ozz runtime skeleton
//                                 (+ ourBoneIndex -> ozzJointIndex mapping)
//   buildOzzClip(aiAnimation*, Skeleton, name)
//                                 Assimp curves -> compressed ozz Animation,
//                                 bound to that skeleton's joints by name
//
// CONVENTIONS (the hard-won part — see animation/info.md):
//  • ozz math is column-vector (Assimp/GL-style). Assimp quaternions feed ozz
//    UNCONJUGATED. Our stored bind SQT rotations are conjugated for the bx
//    row-vector pipeline, so rest-pose keys conjugate BACK ({-x,-y,-z,w}).
//  • ozz Float4x4 column-major memory is byte-identical to bx row-major
//    row-vector memory for the same transform — model matrices from
//    LocalToModelJob store straight into bx float[16], no transpose.
//  • Joints without animation channels get one explicit rest-pose key, so
//    un-animated bones hold bind pose (our long-standing semantics).
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "core/logger.h"

// buildOzzClip consumes aiAnimation — import-side only. Shipping builds
// (ENGINE_WITH_SOURCE_IMPORTERS=0) keep the assimp-free half of this bridge
// (restTransform, buildOzzSkeleton) and compile the clip builder out.
#if ENGINE_WITH_SOURCE_IMPORTERS
#include <assimp/anim.h>
#endif
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace anim {

// Assimp-convention rest SQT for a bone (undo the bx conjugation on rotation).
inline ozz::math::Transform restTransform(const Bone& b) {
    ozz::math::Transform t;
    t.translation = { b.bindPosition.x, b.bindPosition.y, b.bindPosition.z };
    t.rotation    = { -b.bindRotation.x, -b.bindRotation.y, -b.bindRotation.z,
                       b.bindRotation.w };
    t.scale       = { b.bindScale.x, b.bindScale.y, b.bindScale.z };
    return t;
}

// Build the ozz runtime skeleton from an engine Skeleton (parents-first bone
// array) and fill skel.ozz + skel.ozzJointOf. Returns false on failure.
inline bool buildOzzSkeleton(Skeleton& skel) {
    const int n = skel.boneCount();
    if (n == 0) return false;

    // Children lists from parentIndex (bones are parent-before-child).
    std::vector<std::vector<int>> children(n);
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        if (skel.bones[i].parentIndex >= 0) children[skel.bones[i].parentIndex].push_back(i);
        else                                roots.push_back(i);
    }

    ozz::animation::offline::RawSkeleton raw;
    // Recursive fill: our bone i -> raw joint (name + rest transform + kids).
    struct Filler {
        const Skeleton& s;
        const std::vector<std::vector<int>>& kids;
        void fill(ozz::animation::offline::RawSkeleton::Joint& j, int i) const {
            j.name      = s.bones[i].name.c_str();
            j.transform = restTransform(s.bones[i]);
            j.children.resize(kids[i].size());
            for (size_t c = 0; c < kids[i].size(); ++c)
                fill(j.children[c], kids[i][c]);
        }
    } filler{ skel, children };
    raw.roots.resize(roots.size());
    for (size_t r = 0; r < roots.size(); ++r)
        filler.fill(raw.roots[r], roots[r]);

    ozz::animation::offline::SkeletonBuilder builder;
    ozz::unique_ptr<ozz::animation::Skeleton> built = builder(raw);
    if (!built) {
        LOG_ERROR("Anim", "ozz SkeletonBuilder failed (%d bones)", n);
        return false;
    }

    // Map OUR bone order (vertex weights / IBMs) -> ozz joint order, by name.
    skel.ozzJointOf.assign(n, -1);
    for (int j = 0; j < built->num_joints(); ++j) {
        int ours = skel.findBone(built->joint_names()[j]);
        if (ours >= 0) skel.ozzJointOf[ours] = j;
    }
    for (int i = 0; i < n; ++i)
        if (skel.ozzJointOf[i] < 0) {
            LOG_ERROR("Anim", "bone '%s' missing from ozz skeleton",
                      skel.bones[i].name.c_str());
            return false;
        }

    skel.ozz = std::shared_ptr<const ozz::animation::Skeleton>(
        built.release(), ozz::Deleter<ozz::animation::Skeleton>());
    return true;
}

#if ENGINE_WITH_SOURCE_IMPORTERS
// Build a compressed ozz Animation from an Assimp animation, bound to `skel`.
// Track bone-names resolve through the skeleton; unmapped source channels are
// skipped (counted in the clip's diagnostics); joints without channels get one
// rest-pose key.
inline AnimClip buildOzzClip(const aiAnimation* src, const Skeleton& skel,
                             const std::string& fallbackName) {
    AnimClip clip;
    if (!skel.ozz) { LOG_ERROR("Anim", "buildOzzClip: skeleton has no ozz data"); return clip; }

    const double tps      = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 24.0;
    const float  duration = std::max((float)(src->mDuration / tps), 1e-4f);

    const ozz::animation::Skeleton& oskel = *skel.ozz;
    ozz::animation::offline::RawAnimation raw;
    // Name travels INSIDE the ozz Animation (and so through cooked archives).
    raw.duration = duration;
    raw.tracks.resize(oskel.num_joints());

    clip.totalTracks = (int)src->mNumChannels;
    for (unsigned c = 0; c < src->mNumChannels; ++c) {
        const aiNodeAnim* na = src->mChannels[c];
        const int ours = skel.findBone(na->mNodeName.C_Str());
        if (ours < 0) continue;
        auto& track = raw.tracks[(size_t)skel.ozzJointOf[ours]];

        track.translations.reserve(na->mNumPositionKeys);
        for (unsigned k = 0; k < na->mNumPositionKeys; ++k) {
            const auto& kv = na->mPositionKeys[k];
            track.translations.push_back({ (float)(kv.mTime / tps),
                { kv.mValue.x, kv.mValue.y, kv.mValue.z } });
        }
        track.rotations.reserve(na->mNumRotationKeys);
        for (unsigned k = 0; k < na->mNumRotationKeys; ++k) {
            const auto& kv = na->mRotationKeys[k];   // ozz = Assimp convention: no conjugation
            track.rotations.push_back({ (float)(kv.mTime / tps),
                { kv.mValue.x, kv.mValue.y, kv.mValue.z, kv.mValue.w } });
        }
        track.scales.reserve(na->mNumScalingKeys);
        for (unsigned k = 0; k < na->mNumScalingKeys; ++k) {
            const auto& kv = na->mScalingKeys[k];
            track.scales.push_back({ (float)(kv.mTime / tps),
                { kv.mValue.x, kv.mValue.y, kv.mValue.z } });
        }
        // Clamp key times into [0, duration] (Assimp keys can exceed by eps).
        auto clampT = [&](auto& keys) {
            for (auto& key : keys) key.time = std::clamp(key.time, 0.0f, duration);
        };
        clampT(track.translations); clampT(track.rotations); clampT(track.scales);
        ++clip.mappedTracks;
    }

    // Rest-pose key for joints the clip doesn't animate.
    for (int i = 0; i < skel.boneCount(); ++i) {
        auto& track = raw.tracks[(size_t)skel.ozzJointOf[i]];
        const ozz::math::Transform rest = restTransform(skel.bones[i]);
        if (track.translations.empty()) track.translations.push_back({ 0.0f, rest.translation });
        if (track.rotations.empty())    track.rotations.push_back({ 0.0f, rest.rotation });
        if (track.scales.empty())       track.scales.push_back({ 0.0f, rest.scale });
    }

    if (!raw.Validate()) {
        LOG_ERROR("Anim", "RawAnimation validation failed for '%s'", fallbackName.c_str());
        return clip;
    }
    // Resolve the display name BEFORE building: it serializes inside the
    // ozz Animation, so cooked archives carry it too. Mixamo exports junk
    // take names — fall back to the filename stem.
    std::string clipName = src->mName.length ? src->mName.C_Str() : "";
    if (clipName.empty() || clipName == "mixamo.com" || clipName.rfind("Take", 0) == 0)
        clipName = fallbackName;
    raw.name = clipName;

    ozz::animation::offline::AnimationBuilder builder;
    ozz::unique_ptr<ozz::animation::Animation> built = builder(raw);
    if (!built) {
        LOG_ERROR("Anim", "ozz AnimationBuilder failed for '%s'", fallbackName.c_str());
        return clip;
    }

    clip.name     = clipName;
    clip.duration = duration;
    clip.ozz = std::shared_ptr<const ozz::animation::Animation>(
        built.release(), ozz::Deleter<ozz::animation::Animation>());
    return clip;
}
#endif // ENGINE_WITH_SOURCE_IMPORTERS

} // namespace anim
