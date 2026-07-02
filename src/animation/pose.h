#pragma once
// ── Bind-pose & skin-matrix utilities ────────────────────────────────────────
// The two raw-matrix helpers that survive the ozz migration. Everything else
// (hand-rolled channel sampling, SQT poses, blending) is gone — ozz's
// SamplingJob/LocalToModelJob/BlendingJob own that machinery now (see
// systems/animator_system.h and animation/ozz_bridge.h).
//
// These stay on the raw localBindMatrix path deliberately: matrices with
// baked FBX pivots can carry shear a TRS decomposition can't represent, and
// bind-pose skinning must satisfy IBM * world_bind ≈ identity exactly.

#include <bx/math.h>
#include <cassert>
#include <cstring>

#include "animation/skeleton.h"

namespace anim {

// ── Bind-pose world matrices from raw local transforms ──────────────────────
// Bones are topologically sorted: parent always has a lower index.
// outWorldMatrices must hold boneCount * 16 floats.
inline void computeBindPoseWorldMatrices(const Skeleton& skel,
                                         float* outWorldMatrices) {
    const int n = skel.boneCount();
    for (int i = 0; i < n; ++i) {
        float* world = &outWorldMatrices[i * 16];
        if (skel.bones[i].parentIndex >= 0) {
            const float* parentWorld = &outWorldMatrices[skel.bones[i].parentIndex * 16];
            bx::mtxMul(world, skel.bones[i].localBindMatrix, parentWorld);
        } else {
            std::memcpy(world, skel.bones[i].localBindMatrix, 16 * sizeof(float));
        }
    }
}

// ── Final skin matrices (bone palette) ──────────────────────────────────────
// skinMatrix[i] = inverseBindMatrix[i] * worldMatrix[i]
// outSkinMatrices must hold boneCount * 16 floats.
inline void computeSkinMatrices(const Skeleton& skel,
                                const float* worldMatrices,
                                float* outSkinMatrices) {
    const int n = skel.boneCount();
    for (int i = 0; i < n; ++i) {
        const float* world = &worldMatrices[i * 16];
        const float* ibm   = skel.bones[i].inverseBindMatrix;
        float* skin = &outSkinMatrices[i * 16];
        bx::mtxMul(skin, ibm, world);
    }
}

} // namespace anim
