#pragma once
// ── frontend_assimp_skeleton — Assimp's node tree -> the engine's Skeleton ─────
//
// Part of the Assimp front end (audit IMP-01: Assimp's types stay there). It
// was src/animation/assimp_skeleton_loader.h, which tied the animation module
// to one parser (WO-015). Its users:
//
//   * frontend_assimp.cpp, which carries the result into ImportedScene
//   * the two runtime importers WO-018 deletes (async_loader/parse.cpp,
//     assets/importers/assimp_importer.cpp), which also use the legacy
//     buildOzzClip(aiAnimation*) below
//
// Nothing past the front end should include this: build from ImportedScene
// (assets/anim_from_scene.h) instead.
#include <assimp/anim.h>
#include <assimp/scene.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "animation/ozz_bridge.h"
#include <algorithm>
#include <cstdio>

namespace imp::assimp {

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
// with the combined transform, or nullptr if the chain dead-ends. A loop, not
// recursion: the chain's length is the file's.
static const aiNode* collapseHelperChain(const aiNode* node,
                                         aiMatrix4x4& accumulated) {
    accumulated = accumulated * node->mTransformation;
    // While this node has exactly one child and it's a helper, keep collapsing
    while (node->mNumChildren == 1 &&
           isAssimpFbxHelper(node->mChildren[0]->mName.C_Str())) {
        node = node->mChildren[0];
        accumulated = accumulated * node->mTransformation;
    }
    // If this node has exactly one child that is the real bone, return it
    if (node->mNumChildren == 1) {
        accumulated = accumulated * node->mChildren[0]->mTransformation;
        return node->mChildren[0];
    }
    // Multiple children or dead end — shouldn't happen in practice
    return nullptr;
}

// Every node whose subtree (itself included) holds a bone, in one post-order
// pass. This used to be asked per node by walking that node's whole subtree,
// O(n x depth); and like everything here it is a loop, not recursion, because
// the depth is the file's (a deep one overflowed a 512 KB stack, WO-039).
inline std::unordered_set<const aiNode*> nodesAboveBones(const aiNode* root,
                                                         const std::unordered_set<std::string>& boneNames) {
    std::vector<const aiNode*> order;                 // pre-order: parents before children
    std::vector<const aiNode*> stack{root};
    while (!stack.empty()) {
        const aiNode* n = stack.back(); stack.pop_back();
        order.push_back(n);
        for (unsigned c = 0; c < n->mNumChildren; ++c) stack.push_back(n->mChildren[c]);
    }
    std::unordered_set<const aiNode*> has;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {   // children before parents
        const aiNode* n = *it;
        bool h = boneNames.count(n->mName.C_Str()) > 0;
        for (unsigned c = 0; c < n->mNumChildren && !h; ++c) h = has.count(n->mChildren[c]) > 0;
        if (h) has.insert(n);
    }
    return has;
}

// The skeleton's nodes in pre-order: every node with a bone at or below it,
// with $AssimpFbx$ helper chains collapsed into the real bone they lead to.
// An explicit work stack, in the order the recursive version emitted: a Visit
// emits a node and queues its children; a helper child is collapsed when its
// parent is visited and queued as a Real, which emits the bone the chain leads
// to and queues that bone's children.
inline void collectSkeletonNodes(const aiNode* root, int rootParent,
                                 const std::unordered_set<std::string>& boneNames,
                                 std::vector<NodeInfo>& out) {
    const std::unordered_set<const aiNode*> keep = nodesAboveBones(root, boneNames);
    struct Work { const aiNode* node; int parent; bool real; aiMatrix4x4 combined; };
    std::vector<Work> stack{{root, rootParent, false, {}}};
    while (!stack.empty()) {
        const Work w = stack.back(); stack.pop_back();
        if (!keep.count(w.node)) continue;
        const int myIdx = (int)out.size();
        out.push_back({ w.node->mName.C_Str(), w.parent, w.real ? w.combined : w.node->mTransformation });
        std::vector<Work> kids;
        for (unsigned c = 0; c < w.node->mNumChildren; ++c) {
            const aiNode* child = w.node->mChildren[c];
            if (!w.real && isAssimpFbxHelper(child->mName.C_Str())) {
                // Collapse the entire helper chain into one combined transform
                aiMatrix4x4 combined;  // identity
                if (const aiNode* realBone = collapseHelperChain(child, combined))
                    kids.push_back({realBone, myIdx, true, combined});
                // else: dead-end helper chain, skip entirely
            } else {
                // A Real's children are visited as they are: the recursive
                // version recursed straight into them, helpers included.
                kids.push_back({child, myIdx, false, {}});
            }
        }
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
}

} // namespace detail

inline ::Skeleton extractSkeleton(const aiScene* scene) {
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

    // The WHOLE skeleton, however large. This used to keep the first kMaxBones
    // nodes and drop the rest with a line on stderr: weights naming a dropped
    // bone lost it, and a large rig (a MetaHuman has 800+ bones) cooked
    // broken. Whether a rig is too big to SKIN is the consumer's call, made
    // with the real count: the cook back end refuses it by name, and the
    // uncooked preview loads it static (WO-040).
    ::Skeleton skel;
    skel.bones.reserve(nodes.size());

    for (const auto& ni : nodes) {
        ::Bone bone;
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

// ── Animation clips ─────────────────────────────────────────────────────────
// Clip extraction moved to the ozz backbone: anim::buildOzzClip in
// animation/ozz_bridge.h converts Assimp curves into compressed ozz
// Animations bound to the skeleton. The hand-rolled AnimChannel sampler is
// gone (see animation/info.md).

// ── Extract per-vertex bone weights ─────────────────────────────────────────
// For each vertex in a mesh, returns the top 4 bone indices and normalized
// weights. Output arrays must be sized to mesh->mNumVertices.

struct VertexBoneData {
    uint16_t joints[4] = { 0, 0, 0, 0 };   // any bone of the skeleton; the consumer checks its own limit
    float   weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

inline std::vector<VertexBoneData> extractBoneWeights(
        const aiMesh* mesh, const ::Skeleton& skel) {
    std::vector<VertexBoneData> data(mesh->mNumVertices);

    // Track how many influences have been added per vertex
    std::vector<int> influenceCount(mesh->mNumVertices, 0);

    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone* bone = mesh->mBones[b];
        int boneIdx = skel.findBone(bone->mName.C_Str());
        // Not > 255: that silently dropped every influence of a bone past the
        // 256th. A skeleton ImportedScene cannot index (uint16) is refused
        // before weights are read.
        if (boneIdx < 0 || boneIdx > 0xFFFF) continue;

        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            unsigned vid = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vid >= mesh->mNumVertices || weight <= 0.0f) continue;

            int& count = influenceCount[vid];
            if (count < 4) {
                data[vid].joints[count]  = (uint16_t)boneIdx;
                data[vid].weights[count] = weight;
                ++count;
            } else {
                // Replace the smallest existing weight if this one is larger
                int minIdx = 0;
                for (int i = 1; i < 4; ++i)
                    if (data[vid].weights[i] < data[vid].weights[minIdx]) minIdx = i;
                if (weight > data[vid].weights[minIdx]) {
                    data[vid].joints[minIdx]  = (uint16_t)boneIdx;
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

// Build a compressed ozz Animation from an Assimp animation, bound to `skel`.
// LEGACY: for the two runtime importers WO-018 deletes (the uncooked preview's
// async_loader/parse.cpp and assets/importers/assimp_importer.cpp). Everything
// else builds clips from ImportedScene: imp::buildOzzClip(const Clip&, ...)
// in assets/anim_from_scene.h.
// Track bone-names resolve through the skeleton; unmapped source channels are
// skipped (counted in the clip's diagnostics); joints without channels get one
// rest-pose key.
inline AnimClip buildOzzClip(const aiAnimation* src, const ::Skeleton& skel,
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

    // Resolve the display name BEFORE building: it serializes inside the
    // ozz Animation, so cooked archives carry it too. Mixamo exports junk
    // take names — fall back to the filename stem.
    std::string clipName = src->mName.length ? src->mName.C_Str() : "";
    if (clipName.empty() || clipName == "mixamo.com" || clipName.rfind("Take", 0) == 0)
        clipName = fallbackName;
    return anim::finishOzzClip(raw, skel, clipName, clip.mappedTracks, clip.totalTracks);
}

}  // namespace imp::assimp
