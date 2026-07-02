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
    // CONJUGATE (negate xyz): Assimp quaternions are column-vector convention,
    // but bx::mtxFromQuaternion emits column-vector MEMORY into our row-vector
    // pipeline (see aiMat4ToFloat16's transpose) — so an Assimp quaternion
    // recomposes as the INVERSE rotation. The conjugate's matrix is exactly
    // the transpose, making toMatrix(SQT) agree with localBindMatrix. Without
    // this, every rotated bone recomposed inverted — invisible at bind pose
    // (the raw-matrix fallback masked it) and catastrophic on the first real
    // animation (exploded skinned meshes). Clip rotation keys get the same
    // treatment in extractAnimClip — the two MUST stay consistent.
    rot = { -aiRot.x, -aiRot.y, -aiRot.z, aiRot.w };
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

// Assimp's FBX importer decomposes pre-rotation / translation / scaling into
// separate intermediate nodes named  "<bone>_$AssimpFbx$_PreRotation" etc.
// These are NOT real bones — they just split the single FBX local transform
// into orthogonal parts. A typical Mixamo FBX with 68 bones becomes 184+
// nodes, blowing past kMaxBones (128).
//
// Fix: when we encounter a chain of $AssimpFbx$ helper nodes, multiply their
// transforms together and attribute the product to the real bone at the end.
static bool isAssimpFbxHelper(const std::string& name) {
    return name.find("$AssimpFbx$") != std::string::npos;
}

// Walk past a chain of consecutive $AssimpFbx$ helper nodes, accumulating
// their transforms. Returns the first non-helper descendant (the real bone)
// with the combined transform, or nullptr if the chain dead-ends.
static const aiNode* collapseHelperChain(const aiNode* node,
                                         aiMatrix4x4& accumulated) {
    accumulated = accumulated * node->mTransformation;
    // If this node has exactly one child and it's a helper, keep collapsing
    if (node->mNumChildren == 1 &&
        isAssimpFbxHelper(node->mChildren[0]->mName.C_Str())) {
        return collapseHelperChain(node->mChildren[0], accumulated);
    }
    // If this node has exactly one child that is the real bone, return it
    if (node->mNumChildren == 1) {
        accumulated = accumulated * node->mChildren[0]->mTransformation;
        return node->mChildren[0];
    }
    // Multiple children or dead end — shouldn't happen in practice
    return nullptr;
}

inline bool hasDescendantBone(const aiNode* node,
                              const std::unordered_set<std::string>& boneNames) {
    std::vector<const aiNode*> stack;
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        stack.push_back(node->mChildren[c]);
    while (!stack.empty()) {
        const aiNode* n = stack.back(); stack.pop_back();
        if (boneNames.count(n->mName.C_Str())) return true;
        for (unsigned c = 0; c < n->mNumChildren; ++c)
            stack.push_back(n->mChildren[c]);
    }
    return false;
}

inline void collectSkeletonNodes(const aiNode* node, int parentIdx,
                                 const std::unordered_set<std::string>& boneNames,
                                 std::vector<NodeInfo>& out) {
    std::string name = node->mName.C_Str();
    bool isBone = boneNames.count(name) > 0;

    if (!isBone && !hasDescendantBone(node, boneNames)) return;

    int myIdx = (int)out.size();
    out.push_back({ name, parentIdx, node->mTransformation });

    for (unsigned c = 0; c < node->mNumChildren; ++c) {
        const aiNode* child = node->mChildren[c];
        std::string childName = child->mName.C_Str();

        if (isAssimpFbxHelper(childName)) {
            // Collapse the entire helper chain into one combined transform
            aiMatrix4x4 combined;  // identity
            const aiNode* realBone = collapseHelperChain(child, combined);
            if (realBone) {
                // Emit the real bone with the combined transform
                std::string realName = realBone->mName.C_Str();
                bool realIsBone = boneNames.count(realName) > 0;
                if (realIsBone || hasDescendantBone(realBone, boneNames)) {
                    int realIdx = (int)out.size();
                    out.push_back({ realName, myIdx, combined });
                    // Continue recursion from the real bone's children
                    for (unsigned gc = 0; gc < realBone->mNumChildren; ++gc)
                        collectSkeletonNodes(realBone->mChildren[gc], realIdx,
                                             boneNames, out);
                }
            }
            // else: dead-end helper chain, skip entirely
        } else {
            collectSkeletonNodes(child, myIdx, boneNames, out);
        }
    }
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

    // Hard cap: if we still have more nodes than kMaxBones after collapsing
    // helpers, truncate to keep within the fixed-size palette budget.
    if ((int)nodes.size() > kMaxBones) {
        std::fprintf(stderr, "[Skeleton] WARNING: %zu nodes > kMaxBones(%d) "
                     "after helper collapse — truncating\n",
                     nodes.size(), kMaxBones);
        nodes.resize(kMaxBones);
    }

    Skeleton skel;
    skel.bones.reserve(nodes.size());

    for (const auto& ni : nodes) {
        Bone bone;
        bone.name = ni.name;
        bone.parentIndex = ni.parentIndex;
        decomposeAiMatrix(ni.localTransform, bone.bindPosition, bone.bindRotation, bone.bindScale);

        // Store the original local transform losslessly (avoids decompose/recompose error)
        aiMat4ToFloat16(ni.localTransform, bone.localBindMatrix);

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
                // CONJUGATE — must match decomposeAiMatrix (see comment there):
                // Assimp quats recompose inverted in our row-vector pipeline.
                const auto& q = na->mRotationKeys[k].mValue;
                ch.values.push_back(-q.x);
                ch.values.push_back(-q.y);
                ch.values.push_back(-q.z);
                ch.values.push_back( q.w);
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
