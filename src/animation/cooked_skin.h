#pragma once
// ── cooked_skin — decode a v3 cooked mesh's skinned payload ──────────────────
// Bones + the opaque ozz skeleton/clip archives embedded in a MeshAsset,
// decoded into runtime Skeleton/AnimClip objects. Pure CPU, no Assimp, no
// bgfx — safe on any worker thread and in shipping builds. Shared by the
// editor's AsyncLoader (async_loader/parse.cpp) and AssetService's cooked
// streaming path so both flows produce identical runtime data.
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "core/logger.h"

#include <assetlib/mesh_asset.h>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/memory/unique_ptr.h>

#include <cstring>
#include <vector>

namespace anim {

// Rebuild the runtime Skeleton (bones + ozz runtime skeleton + our-bone →
// ozz-joint mapping) from a cooked mesh. Returns a skeleton with .ozz null
// if the archive blob is unreadable — caller decides the fallback.
inline Skeleton decodeCookedSkeleton(const assetlib::MeshAsset& asset) {
    Skeleton skel;
    skel.bones.reserve(asset.bones.size());
    for (const auto& cb : asset.bones) {
        Bone b;
        b.name        = cb.name;
        b.parentIndex = cb.parentIndex;
        b.bindPosition = {cb.bindPosition[0], cb.bindPosition[1],
                          cb.bindPosition[2]};
        b.bindRotation = {cb.bindRotation[0], cb.bindRotation[1],
                          cb.bindRotation[2], cb.bindRotation[3]};
        b.bindScale    = {cb.bindScale[0], cb.bindScale[1], cb.bindScale[2]};
        std::memcpy(b.inverseBindMatrix, cb.inverseBindMatrix, 64);
        std::memcpy(b.localBindMatrix,   cb.localBindMatrix,   64);
        skel.bones.push_back(std::move(b));
    }

    // ozz skeleton from the opaque archive blob.
    {
        ozz::io::MemoryStream ms;
        ms.Write(asset.skeletonBlob.data(), asset.skeletonBlob.size());
        ms.Seek(0, ozz::io::Stream::kSet);
        ozz::io::IArchive ar(&ms);
        if (ar.TestTag<ozz::animation::Skeleton>()) {
            auto sk = ozz::make_unique<ozz::animation::Skeleton>();
            ar >> *sk;
            skel.ozz = std::shared_ptr<const ozz::animation::Skeleton>(
                sk.release(), ozz::Deleter<ozz::animation::Skeleton>());
        }
    }
    if (skel.ozz) {
        // our-bone -> ozz-joint mapping by name (cheap).
        const auto names = skel.ozz->joint_names();
        skel.ozzJointOf.assign(skel.bones.size(), 0);
        for (size_t i = 0; i < skel.bones.size(); ++i)
            for (int j = 0; j < (int)names.size(); ++j)
                if (skel.bones[i].name == names[j]) {
                    skel.ozzJointOf[i] = j;
                    break;
                }
    }
    // findBone() answers from boneMap — WITHOUT this, every name lookup
    // returns -1 and standalone clips bind 0 tracks ("wrong rig?"). The
    // old inline decode never built it either; cooked-clip cache hits
    // masked the bug until the first fresh bind against a cooked skeleton.
    skel.buildBoneMap();
    return skel;
}

// Deserialize every clip archive embedded in a cooked mesh.
inline std::vector<AnimClip> decodeCookedClips(const assetlib::MeshAsset& asset) {
    std::vector<AnimClip> out;
    for (const auto& cc : asset.clips) {
        ozz::io::MemoryStream ms;
        ms.Write(cc.blob.data(), cc.blob.size());
        ms.Seek(0, ozz::io::Stream::kSet);
        ozz::io::IArchive ar(&ms);
        if (!ar.TestTag<ozz::animation::Animation>()) continue;
        auto a = ozz::make_unique<ozz::animation::Animation>();
        ar >> *a;
        AnimClip clip;
        clip.name         = a->name();
        clip.duration     = a->duration();
        clip.mappedTracks = cc.mappedTracks;
        clip.totalTracks  = cc.totalTracks;
        clip.ozz = std::shared_ptr<const ozz::animation::Animation>(
            a.release(), ozz::Deleter<ozz::animation::Animation>());
        out.push_back(std::move(clip));
    }
    return out;
}

} // namespace anim
