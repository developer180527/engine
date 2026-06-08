#pragma once

#include <bx/math.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
};

struct Skeleton {
    std::vector<Bone>                          bones;
    std::unordered_map<std::string, int>       boneMap;

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
