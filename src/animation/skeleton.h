#pragma once

#include <bx/math.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ozz::animation { class Skeleton; }   // fwd — the ozz runtime skeleton

static constexpr int kMaxBones = 128;

struct Bone {
    std::string    name;
    int            parentIndex = -1;
    bx::Vec3       bindPosition { 0.0f, 0.0f, 0.0f };
    bx::Quaternion bindRotation { 0.0f, 0.0f, 0.0f, 1.0f };
    bx::Vec3       bindScale    { 1.0f, 1.0f, 1.0f };
    float          inverseBindMatrix[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    // Original local bind-pose transform (row-major, bx convention).
    // Avoids the lossy decompose→SQT→recompose cycle that introduces
    // accumulated error down long bone chains when FBX pre-rotation/
    // post-rotation data is baked in (PRESERVE_PIVOTS=false).
    float          localBindMatrix[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
};

struct Skeleton {
    std::vector<Bone>                          bones;
    std::unordered_map<std::string, int>       boneMap;

    // ── ozz backbone (built once at import by anim::buildOzzSkeleton) ──────
    // `ozz` is the runtime skeleton the SamplingJob/LocalToModelJob consume.
    // ozz orders joints its own way (parents-first, its traversal), so
    // `ozzJointOf[ourBoneIndex]` maps OUR bone order (what vertex weights and
    // IBMs index) to ozz joint indices. shared_ptr keeps Skeleton copyable —
    // the ozz skeleton is immutable shared asset data.
    std::shared_ptr<const ozz::animation::Skeleton> ozz;
    std::vector<int>                                ozzJointOf;

    int findBone(const std::string& name) const {
        auto it = boneMap.find(name);
        return it != boneMap.end() ? it->second : -1;
    }

    void buildBoneMap() {
        boneMap.clear();
        for (int i = 0; i < (int)bones.size(); ++i)
            boneMap[bones[i].name] = i;
    }

    int boneCount() const { return (int)bones.size(); }
};
