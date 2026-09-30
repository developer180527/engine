#pragma once
// ── ozz bridge — engine skeletons and keys -> ozz runtime structures ──────────
// The seam between the engine's animation types and the ozz backbone. It knows
// no source format (WO-015: it used to take Assimp's aiAnimation):
//
//   buildOzzSkeleton(Skeleton&)   engine Skeleton -> ozz runtime skeleton
//                                 (+ ourBoneIndex -> ozzJointIndex mapping)
//   finishOzzClip(RawAnimation&, Skeleton, name, ...)
//                                 source keys -> compressed ozz Animation
//
// Clips are built from ImportedScene by imp::buildOzzClip
// (assets/anim_from_scene.h), which fills the RawAnimation and calls
// finishOzzClip.
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

// The format-independent half of building a clip: give every joint the clip
// does not animate one rest-pose key, validate, and build the compressed ozz
// Animation. `raw.tracks` must already be sized to the ozz skeleton's joints and
// hold the source keys (seconds; rotations NOT conjugated — ozz uses the source
// convention). Every clip builder ends here (imp::buildOzzClip, and the Assimp
// front end's legacy one), so all of them build clips identically.
inline AnimClip finishOzzClip(ozz::animation::offline::RawAnimation& raw, const Skeleton& skel,
                              const std::string& name, int mappedTracks, int totalTracks) {
    AnimClip clip;
    clip.mappedTracks = mappedTracks;
    clip.totalTracks  = totalTracks;
    for (int i = 0; i < skel.boneCount(); ++i) {
        auto& track = raw.tracks[(size_t)skel.ozzJointOf[i]];
        const ozz::math::Transform rest = restTransform(skel.bones[i]);
        if (track.translations.empty()) track.translations.push_back({ 0.0f, rest.translation });
        if (track.rotations.empty())    track.rotations.push_back({ 0.0f, rest.rotation });
        if (track.scales.empty())       track.scales.push_back({ 0.0f, rest.scale });
    }
    if (!raw.Validate()) {
        LOG_ERROR("Anim", "RawAnimation validation failed for '%s'", name.c_str());
        return clip;
    }
    raw.name = name;   // serializes inside the ozz Animation, so cooked archives carry it
    ozz::animation::offline::AnimationBuilder builder;
    ozz::unique_ptr<ozz::animation::Animation> built = builder(raw);
    if (!built) {
        LOG_ERROR("Anim", "ozz AnimationBuilder failed for '%s'", name.c_str());
        return clip;
    }
    clip.name     = name;
    clip.duration = raw.duration;
    clip.ozz = std::shared_ptr<const ozz::animation::Animation>(
        built.release(), ozz::Deleter<ozz::animation::Animation>());
    return clip;
}

// Bind a clip whose keys are held BY BONE NAME (keys.tracks[i] animates the
// bone named trackBones[i]) to `skel`, which has its ozz data: each track moves
// to the joint of that name, a track naming no bone is skipped and counted, and
// finishOzzClip does the rest. Every clip is built through here: straight from
// an ImportedScene (imp::buildOzzClip) and from a cooked clip (ClipLibrary), so
// the two cannot differ (WO-016).
inline AnimClip bindRawClip(const std::vector<std::string>& trackBones,
                            const ozz::animation::offline::RawAnimation& keys,
                            const Skeleton& skel) {
    if (!skel.ozz) { LOG_ERROR("Anim", "bindRawClip: skeleton has no ozz data"); return {}; }
    ozz::animation::offline::RawAnimation raw;
    raw.duration = keys.duration;
    raw.tracks.resize((size_t)skel.ozz->num_joints());
    int mapped = 0;
    for (size_t i = 0; i < trackBones.size() && i < keys.tracks.size(); ++i) {
        const int ours = skel.findBone(trackBones[i].c_str());
        if (ours < 0) continue;
        raw.tracks[(size_t)skel.ozzJointOf[ours]] = keys.tracks[i];
        ++mapped;
    }
    return finishOzzClip(raw, skel, keys.name.c_str(), mapped, (int)trackBones.size());
}

} // namespace anim
