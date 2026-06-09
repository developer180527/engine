#pragma once

#include <bx/math.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "animation/animation_math.h"

struct BoneTransform {
    bx::Vec3       position { 0.0f, 0.0f, 0.0f };
    bx::Quaternion rotation { 0.0f, 0.0f, 0.0f, 1.0f };
    bx::Vec3       scale    { 1.0f, 1.0f, 1.0f };

    void toMatrix(float out[16]) const {
        float rotMtx[16];
        bx::mtxFromQuaternion(rotMtx, rotation);
        float scaleMtx[16];
        bx::mtxScale(scaleMtx, scale.x, scale.y, scale.z);
        float sr[16];
        bx::mtxMul(sr, scaleMtx, rotMtx);
        float transMtx[16];
        bx::mtxTranslate(transMtx, position.x, position.y, position.z);
        bx::mtxMul(out, sr, transMtx);
    }
};

struct Pose {
    std::vector<BoneTransform> locals;
    // Per-bone flags tracking which properties were overridden by animation.
    // Bones with no animation channels keep the raw localBindMatrix to avoid
    // the lossy decompose→SQT→recompose round-trip.
    std::vector<uint8_t>       animated; // bitmask: 1=pos, 2=rot, 4=scl

    void resize(int boneCount) {
        locals.resize(boneCount);
        animated.assign(boneCount, 0);
    }
    int  boneCount() const     { return (int)locals.size(); }
    bool isAnimated(int i) const {
        return i < (int)animated.size() && animated[i] != 0;
    }
};

namespace anim {

// ── Bind pose from skeleton ─────────────────────────────────────────────────

inline Pose bindPose(const Skeleton& skel) {
    Pose p;
    p.resize(skel.boneCount());
    for (int i = 0; i < skel.boneCount(); ++i) {
        p.locals[i].position = skel.bones[i].bindPosition;
        p.locals[i].rotation = skel.bones[i].bindRotation;
        p.locals[i].scale    = skel.bones[i].bindScale;
    }
    return p;
}

// ── Sample a clip at a given time ───────────────────────────────────────────
// Starts from bind pose, then overwrites bones that have channels.

inline Pose sampleClip(const AnimClip& clip, const Skeleton& skel, float time) {
    Pose pose = bindPose(skel);

    for (const AnimChannel& ch : clip.channels) {
        if (ch.boneIndex < 0 || ch.boneIndex >= pose.boneCount()) continue;

        BoneTransform& bt = pose.locals[ch.boneIndex];
        float tmp[4];
        sampleChannel(ch, time, tmp);

        switch (ch.property) {
        case AnimProperty::Translation:
            bt.position = { tmp[0], tmp[1], tmp[2] };
            pose.animated[ch.boneIndex] |= 1;
            break;
        case AnimProperty::Rotation:
            bt.rotation = { tmp[0], tmp[1], tmp[2], tmp[3] };
            pose.animated[ch.boneIndex] |= 2;
            break;
        case AnimProperty::Scale:
            bt.scale = { tmp[0], tmp[1], tmp[2] };
            pose.animated[ch.boneIndex] |= 4;
            break;
        }
    }
    return pose;
}

// ── Blend two poses ─────────────────────────────────────────────────────────
// Per-bone: lerp position, slerp rotation, lerp scale.

inline Pose blendPoses(const Pose& a, const Pose& b, float alpha) {
    assert(a.boneCount() == b.boneCount());
    Pose result;
    result.resize(a.boneCount());

    for (int i = 0; i < a.boneCount(); ++i) {
        result.locals[i].position = lerpVec3(a.locals[i].position, b.locals[i].position, alpha);
        result.locals[i].rotation = bx::slerp(a.locals[i].rotation, b.locals[i].rotation, alpha);
        result.locals[i].scale    = lerpVec3(a.locals[i].scale, b.locals[i].scale, alpha);
        // A blended bone is animated if either source was animated
        result.animated[i] = a.animated[i] | b.animated[i];
    }
    return result;
}

// ── Additive blend ──────────────────────────────────────────────────────────
// result = base + alpha * (additive - reference)
// Used for breathing overlays, hit reactions, etc.

inline Pose additiveBlend(const Pose& base, const Pose& additive,
                          const Pose& reference, float alpha) {
    assert(base.boneCount() == additive.boneCount());
    assert(base.boneCount() == reference.boneCount());
    Pose result;
    result.resize(base.boneCount());

    for (int i = 0; i < base.boneCount(); ++i) {
        // Position: base + alpha * (additive - reference)
        bx::Vec3 dp = bx::sub(additive.locals[i].position, reference.locals[i].position);
        result.locals[i].position = bx::add(base.locals[i].position, bx::mul(dp, alpha));

        // Rotation: base * slerp(identity, inv(reference) * additive, alpha)
        bx::Quaternion refInv = { -reference.locals[i].rotation.x,
                                  -reference.locals[i].rotation.y,
                                  -reference.locals[i].rotation.z,
                                   reference.locals[i].rotation.w };
        bx::Quaternion delta = bx::mul(refInv, additive.locals[i].rotation);
        bx::Quaternion identity { 0.0f, 0.0f, 0.0f, 1.0f };
        bx::Quaternion scaled = bx::slerp(identity, delta, alpha);
        result.locals[i].rotation = bx::normalize(bx::mul(base.locals[i].rotation, scaled));

        // Scale: base * lerp(1, additive / reference, alpha)
        auto safeDiv = [](float a_, float b_) { return b_ > 1e-6f ? a_ / b_ : 1.0f; };
        bx::Vec3 ds = { safeDiv(additive.locals[i].scale.x, reference.locals[i].scale.x),
                        safeDiv(additive.locals[i].scale.y, reference.locals[i].scale.y),
                        safeDiv(additive.locals[i].scale.z, reference.locals[i].scale.z) };
        bx::Vec3 one { 1.0f, 1.0f, 1.0f };
        bx::Vec3 sl = lerpVec3(one, ds, alpha);
        result.locals[i].scale = { base.locals[i].scale.x * sl.x,
                                   base.locals[i].scale.y * sl.y,
                                   base.locals[i].scale.z * sl.z };
        // Additive blend produces animated bones from any animated source
        result.animated[i] = base.animated[i] | additive.animated[i] | reference.animated[i];
    }
    return result;
}

// ── Compute world matrices from local pose ──────────────────────────────────
// Bones are topologically sorted: parent always has a lower index.
// outWorldMatrices must hold boneCount * 16 floats.
//
// For bones that were NOT animated (pose.animated[i] == 0), use the lossless
// localBindMatrix directly instead of the decompose→SQT→recompose path.
// This avoids accumulated floating-point error that grows catastrophically
// down long bone chains when FBX pre-rotation data is baked in.

inline void computeWorldMatrices(const Skeleton& skel, const Pose& pose,
                                 float* outWorldMatrices) {
    assert(skel.boneCount() == pose.boneCount());
    const int n = skel.boneCount();

    for (int i = 0; i < n; ++i) {
        const float* localMtx;
        float sqtMtx[16];

        if (pose.isAnimated(i)) {
            // Bone has animation channels — use SQT→matrix (animation provides clean data)
            pose.locals[i].toMatrix(sqtMtx);
            localMtx = sqtMtx;
        } else {
            // No animation channels — use the original bind-pose matrix losslessly
            localMtx = skel.bones[i].localBindMatrix;
        }

        float* world = &outWorldMatrices[i * 16];
        if (skel.bones[i].parentIndex >= 0) {
            const float* parentWorld = &outWorldMatrices[skel.bones[i].parentIndex * 16];
            bx::mtxMul(world, localMtx, parentWorld);
        } else {
            std::memcpy(world, localMtx, 16 * sizeof(float));
        }
    }
}

// ── Compute bind-pose world matrices directly from raw local transforms ─────
// Uses localBindMatrix exclusively — no SQT decomposition round-trip.
// Guarantees skin = IBM * World_bind ≈ identity.

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

// ── Compute final skin matrices (bone palette) ─────────────────────────────
// skinMatrix[i] = inverseBindMatrix[i] * worldMatrix[i]
// This transforms vertices from mesh-space to bone-space, then to world-space.
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
