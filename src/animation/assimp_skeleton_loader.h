#pragma once

#include <assimp/scene.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "animation/skeleton.h"
#include "animation/animation_clip.h"

namespace anim {

// ── aiMatrix4x4 -> float[16] (row-major, bgfx convention) ──────────────────

inline void aiMat4ToFloat16(const aiMatrix4x4& m, float out[16]) {
    // Assimp is row-major, bgfx is row-major — direct transpose of the
    // column-major memory layout Assimp uses internally.
    out[ 0] = m.a1; out[ 1] = m.b1; out[ 2] = m.c1; out[ 3] = m.d1;
    out[ 4] = m.a2; out[ 5] = m.b2; out[ 6] = m.c2; out[ 7] = m.d2;
    out[ 8] = m.a3; out[ 9] = m.b3; out[10] = m.c3; out[11] = m.d3;
    out[12] = m.a4; out[13] = m.b4; out[14] = m.c4; out[15] = m.d4;
}

// ── Decompose aiMatrix4x4 to SQT ───────────────────────────────────────────

inline void decomposeAiMatrix(const aiMatrix4x4& m,
                              bx::Vec3& pos, bx::Quaternion& rot, bx::Vec3& scl) {
    aiVector3D aiPos, aiScl;
    aiQuaternion aiRot;
    m.Decompose(aiScl, aiRot, aiPos);
    pos = { aiPos.x, aiPos.y, aiPos.z };
    rot = { aiRot.x, aiRot.y, aiRot.z, aiRot.w };
    scl = { aiScl.x, aiScl.y, aiScl.z };
}

// ── Collect all bone names from all meshes in the scene ─────────────────────

inline std::unordered_set<std::string> collectBoneNames(const aiScene* scene) {
    std::unordered_set<std::string> names;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            names.insert(mesh->mBones[b]->mName.C_Str());
    }
    return names;
}

// ── Build skeleton from aiNode tree + aiBone data ───────────────────────────
// Walks the node tree, includes nodes that are bones (matched by name) plus
// their ancestors up to the root. Bones are topologically sorted (parent
// index always < child index).

namespace detail {

struct NodeInfo {
    std::string name;
    int         parentIndex;
    aiMatrix4x4 localTransform;
};

inline void collectSkeletonNodes(const aiNode* node, int parentIdx,
                                 const std::unordered_set<std::string>& boneNames,
                                 std::vector<NodeInfo>& out) {
    std::string name = node->mName.C_Str();

    // Always include nodes that are bones. Also include intermediate nodes
    // (they may be needed for hierarchy correctness — pruned later if unused).
    bool isBone = boneNames.count(name) > 0;

    // Check if any descendant is a bone
    bool hasDescendantBone = false;
    if (!isBone) {
        std::vector<const aiNode*> stack;
        for (unsigned c = 0; c < node->mNumChildren; ++c)
            stack.push_back(node->mChildren[c]);
        while (!stack.empty()) {
            const aiNode* n = stack.back(); stack.pop_back();
            if (boneNames.count(n->mName.C_Str())) { hasDescendantBone = true; break; }
            for (unsigned c = 0; c < n->mNumChildren; ++c)
                stack.push_back(n->mChildren[c]);
        }
    }

    if (!isBone && !hasDescendantBone) return;

    int myIdx = (int)out.size();
    out.push_back({ name, parentIdx, node->mTransformation });

    for (unsigned c = 0; c < node->mNumChildren; ++c)
        collectSkeletonNodes(node->mChildren[c], myIdx, boneNames, out);
}

} // namespace detail

inline Skeleton extractSkeleton(const aiScene* scene) {
    auto boneNames = collectBoneNames(scene);
    if (boneNames.empty()) return {};

    // Build inverse bind matrix map from aiBone data
    std::unordered_map<std::string, aiMatrix4x4> ibmMap;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            ibmMap[bone->mName.C_Str()] = bone->mOffsetMatrix;
        }
    }

    // Walk node tree to collect skeleton hierarchy
    std::vector<detail::NodeInfo> nodes;
    detail::collectSkeletonNodes(scene->mRootNode, -1, boneNames, nodes);

    Skeleton skel;
    skel.bones.reserve(nodes.size());

    for (const auto& ni : nodes) {
        Bone bone;
        bone.name = ni.name;
        bone.parentIndex = ni.parentIndex;
        decomposeAiMatrix(ni.localTransform, bone.bindPosition, bone.bindRotation, bone.bindScale);

        // Use inverse bind matrix from aiBone if available, otherwise identity
        auto it = ibmMap.find(ni.name);
        if (it != ibmMap.end()) {
            aiMat4ToFloat16(it->second, bone.inverseBindMatrix);
        }

        skel.bones.push_back(std::move(bone));
    }
    skel.buildBoneMap();
    return skel;
}

// ── Extract animation clips ─────────────────────────────────────────────────

inline AnimClip extractAnimClip(const aiAnimation* anim, const Skeleton& skel) {
    AnimClip clip;
    clip.name = anim->mName.C_Str();
    clip.ticksPerSecond = (anim->mTicksPerSecond > 0.0)
        ? (float)anim->mTicksPerSecond : 24.0f;
    clip.duration = (float)(anim->mDuration / clip.ticksPerSecond);

    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim* na = anim->mChannels[c];
        int boneIdx = skel.findBone(na->mNodeName.C_Str());
        if (boneIdx < 0) continue;

        // Position keys
        if (na->mNumPositionKeys > 0) {
            AnimChannel ch;
            ch.boneIndex = boneIdx;
            ch.property = AnimProperty::Translation;
            ch.interpolation = AnimInterp::Linear;
            ch.timestamps.reserve(na->mNumPositionKeys);
            ch.values.reserve(na->mNumPositionKeys * 3);
            for (unsigned k = 0; k < na->mNumPositionKeys; ++k) {
                ch.timestamps.push_back((float)(na->mPositionKeys[k].mTime / clip.ticksPerSecond));
                const auto& v = na->mPositionKeys[k].mValue;
                ch.values.push_back(v.x);
                ch.values.push_back(v.y);
                ch.values.push_back(v.z);
            }
            clip.channels.push_back(std::move(ch));
        }

        // Rotation keys
        if (na->mNumRotationKeys > 0) {
            AnimChannel ch;
            ch.boneIndex = boneIdx;
            ch.property = AnimProperty::Rotation;
            ch.interpolation = AnimInterp::Linear;
            ch.timestamps.reserve(na->mNumRotationKeys);
            ch.values.reserve(na->mNumRotationKeys * 4);
            for (unsigned k = 0; k < na->mNumRotationKeys; ++k) {
                ch.timestamps.push_back((float)(na->mRotationKeys[k].mTime / clip.ticksPerSecond));
                const auto& q = na->mRotationKeys[k].mValue;
                ch.values.push_back(q.x);
                ch.values.push_back(q.y);
                ch.values.push_back(q.z);
                ch.values.push_back(q.w);
            }
            clip.channels.push_back(std::move(ch));
        }

        // Scale keys
        if (na->mNumScalingKeys > 0) {
            AnimChannel ch;
            ch.boneIndex = boneIdx;
            ch.property = AnimProperty::Scale;
            ch.interpolation = AnimInterp::Linear;
            ch.timestamps.reserve(na->mNumScalingKeys);
            ch.values.reserve(na->mNumScalingKeys * 3);
            for (unsigned k = 0; k < na->mNumScalingKeys; ++k) {
                ch.timestamps.push_back((float)(na->mScalingKeys[k].mTime / clip.ticksPerSecond));
                const auto& v = na->mScalingKeys[k].mValue;
                ch.values.push_back(v.x);
                ch.values.push_back(v.y);
                ch.values.push_back(v.z);
            }
            clip.channels.push_back(std::move(ch));
        }
    }

    return clip;
}

// ── Extract all clips from a scene ──────────────────────────────────────────

inline std::vector<AnimClip> extractAllClips(const aiScene* scene, const Skeleton& skel) {
    std::vector<AnimClip> clips;
    clips.reserve(scene->mNumAnimations);
    for (unsigned i = 0; i < scene->mNumAnimations; ++i)
        clips.push_back(extractAnimClip(scene->mAnimations[i], skel));
    return clips;
}

// ── Extract per-vertex bone weights ─────────────────────────────────────────
// For each vertex in a mesh, returns the top 4 bone indices and normalized
// weights. Output arrays must be sized to mesh->mNumVertices.

struct VertexBoneData {
    uint8_t joints[4]  = { 0, 0, 0, 0 };
    float   weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

inline std::vector<VertexBoneData> extractBoneWeights(
        const aiMesh* mesh, const Skeleton& skel) {
    std::vector<VertexBoneData> data(mesh->mNumVertices);

    // Track how many influences have been added per vertex
    std::vector<int> influenceCount(mesh->mNumVertices, 0);

    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone* bone = mesh->mBones[b];
        int boneIdx = skel.findBone(bone->mName.C_Str());
        if (boneIdx < 0 || boneIdx > 255) continue;

        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            unsigned vid = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vid >= mesh->mNumVertices || weight <= 0.0f) continue;

            int& count = influenceCount[vid];
            if (count < 4) {
                data[vid].joints[count]  = (uint8_t)boneIdx;
                data[vid].weights[count] = weight;
                ++count;
            } else {
                // Replace the smallest existing weight if this one is larger
                int minIdx = 0;
                for (int i = 1; i < 4; ++i)
                    if (data[vid].weights[i] < data[vid].weights[minIdx]) minIdx = i;
                if (weight > data[vid].weights[minIdx]) {
                    data[vid].joints[minIdx]  = (uint8_t)boneIdx;
                    data[vid].weights[minIdx] = weight;
                }
            }
        }
    }

    // Normalize weights to sum to 1.0
    for (auto& vd : data) {
        float sum = vd.weights[0] + vd.weights[1] + vd.weights[2] + vd.weights[3];
        if (sum > 1e-6f) {
            float inv = 1.0f / sum;
            vd.weights[0] *= inv;
            vd.weights[1] *= inv;
            vd.weights[2] *= inv;
            vd.weights[3] *= inv;
        }
    }

    return data;
}

} // namespace anim
