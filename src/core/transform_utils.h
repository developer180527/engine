#pragma once
// ── transform_utils — SRT matrix math, with no ECS ──────────────────────────
//
// Pure functions over matrices and poses. NOTHING HERE TAKES A flecs::entity,
// and that is the point rather than a coincidence: src/core/info.md's rule for
// this layer is that it "should compile in a unit test with no GPU, no ECS, no
// filesystem", and until 2026-09-16 this header broke that rule by including
// <flecs.h> for a set of hierarchy walkers.
//
// Those walkers — hierarchyDepth, safeReparent, getWorldMatrix,
// getWorldMatrixLerp, getWorldMatrixLerpFrom, isAncestorOf — now live in
// components/transform_hierarchy.h, which may use the ECS. They are the half
// that genuinely needs it: a parent chain IS an ECS relationship. What stayed
// here is the half that never did, and which a cook tool, a headless server or
// a unit test can use without linking flecs at all.
#include <bx/math.h>
#include <cstring>
#include <cmath>
#include "core/transform.h"
#include "core/logger.h"
// A plain POD struct (bx types only), not an ECS header — core may include it.
#include "components/prev_transform.h"

// ── quatFromMatrix ─────────────────────────────────────────────────────────
// Extract quaternion from the upper-left 3x3 of a bgfx row-major matrix.
// Row-major layout: R[row][col] = m[row*4 + col]
//   R[0][0..2] = right,  R[1][0..2] = up,  R[2][0..2] = back
// Uses Shoemake (1994) stable branch selection.
inline bx::Quaternion quatFromMatrix(const float m[16]) {
    float t = m[0] + m[5] + m[10]; // trace
    float x, y, z, w;
    if (t > 0.0f) {
        float s = 2.0f * sqrtf(t + 1.0f); // s = 4w
        w = 0.25f * s;
        x = (m[9]  - m[6]) / s; // R[2][1] - R[1][2]
        y = (m[2]  - m[8]) / s; // R[0][2] - R[2][0]
        z = (m[4]  - m[1]) / s; // R[1][0] - R[0][1]
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = 2.0f * sqrtf(1.0f + m[0] - m[5] - m[10]);
        w = (m[9]  - m[6]) / s;
        x = 0.25f * s;
        y = (m[1]  + m[4]) / s;
        z = (m[2]  + m[8]) / s;
    } else if (m[5] > m[10]) {
        float s = 2.0f * sqrtf(1.0f + m[5] - m[0] - m[10]);
        w = (m[2]  - m[8]) / s;
        x = (m[1]  + m[4]) / s;
        y = 0.25f * s;
        z = (m[6]  + m[9]) / s;
    } else {
        float s = 2.0f * sqrtf(1.0f + m[10] - m[0] - m[5]);
        w = (m[4]  - m[1]) / s;
        x = (m[2]  + m[8]) / s;
        y = (m[6]  + m[9]) / s;
        z = 0.25f * s;
    }
    return bx::normalize(bx::Quaternion{x, y, z, w});
}

// ── decomposeMatrix ────────────────────────────────────────────────────────
// Decompose a bgfx row-major SRT matrix into position, rotation, scale.
// Scale is extracted as the magnitude of each basis row; rotation is
// extracted after dividing out scale (normalized rows).
inline void decomposeMatrix(const float m[16],
                             bx::Vec3& pos,
                             bx::Quaternion& rot,
                             bx::Vec3& scale) {
    pos = {m[12], m[13], m[14]};
    scale.x = bx::length({m[0], m[1], m[2]});
    scale.y = bx::length({m[4], m[5], m[6]});
    scale.z = bx::length({m[8], m[9], m[10]});
    if (scale.x < 1e-6f) scale.x = 1.0f;
    if (scale.y < 1e-6f) scale.y = 1.0f;
    if (scale.z < 1e-6f) scale.z = 1.0f;
    // Build normalized rotation matrix
    float r[16] = {
        m[0]/scale.x, m[1]/scale.x, m[2]/scale.x,  0,
        m[4]/scale.y, m[5]/scale.y, m[6]/scale.y,  0,
        m[8]/scale.z, m[9]/scale.z, m[10]/scale.z, 0,
        0, 0, 0, 1
    };
    rot = quatFromMatrix(r);
}

// ── nlerpQuat / localMatrixLerp ────────────────────────────────────────────
// The render side of the fixed-timestep loop: each local pose is
// nlerp(PrevTransform, Transform, alpha). Entities without PrevTransform
// (editor world, cameras, alpha == 1) use their current transform, so this is
// safe as the universal extraction path.
inline bx::Quaternion nlerpQuat(const bx::Quaternion& a, const bx::Quaternion& b,
                                float t) {
    // Shortest arc: flip when the hemispheres disagree.
    const float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    const float s = d < 0.0f ? -1.0f : 1.0f;
    bx::Quaternion q = {
        bx::lerp(a.x, s*b.x, t), bx::lerp(a.y, s*b.y, t),
        bx::lerp(a.z, s*b.z, t), bx::lerp(a.w, s*b.w, t) };
    return bx::normalize(q);
}

// The interpolated LOCAL matrix from components already in hand. `prev` may be
// null (no PrevTransform, or alpha == 1). Takes both components rather than an
// entity on purpose: a caller iterating a query has them, and looking either one
// up again is a per-entity sparse-set probe — see the measurements on
// getWorldMatrixLerpFrom, in components/transform_hierarchy.h.
inline void localMatrixLerp(const Transform& t, const PrevTransform* prev,
                            float alpha, float local[16]) {
    const PrevTransform* p = alpha < 1.0f ? prev : nullptr;
    if (!p) {
        t.getMatrix(local);
    } else {
        Transform tmp = t;
        tmp.position = bx::lerp(p->position, t.position, alpha);
        tmp.rotation = nlerpQuat(p->rotation, t.rotation, alpha);
        tmp.scale    = bx::lerp(p->scale, t.scale, alpha);
        tmp.getMatrix(local);
    }
}

// ── safeInvert ─────────────────────────────────────────────────────────────
// Invert an affine SRT matrix, guarding against a singular linear part
// (zero / near-zero scale from scripts or loaded files). Returns false and
// writes identity when the upper-left 3x3 is non-invertible.
inline bool safeInvert(float out[16], const float m[16], float eps = 1e-8f) {
    float det3 = m[0]*(m[5]*m[10] - m[6]*m[9])
               - m[1]*(m[4]*m[10] - m[6]*m[8])
               + m[2]*(m[4]*m[9]  - m[5]*m[8]);
    if (std::fabs(det3) < eps) { bx::mtxIdentity(out); return false; }
    bx::mtxInverse(out, m);
    return true;
}

// ── worldToLocalMatrix ─────────────────────────────────────────────────────
// The one place a world pose is brought back under a parent.
//
// FOUR SITES HAND-ROLLED THIS AND ALL FOUR IGNORED safeInvert's BOOL —
// jolt_plugin.h (bodies and characters), gizmo.h, hierarchy_panel.h. On a
// singular parent every one of them silently used IDENTITY as the inverse,
// which does not mean "no parent": it means the child's LOCAL transform is
// overwritten with its WORLD one, so the entity jumps by the parent's full
// pose. A child under a parent 100 units away teleports 100 units, once, with
// no diagnostic.
//
// ── WHY IT CLAMPS RATHER THAN DECLINES ─────────────────────────────────────
// A previous draft proposed declining — leave the local pose alone and freeze
// the entity. That leaves it permanently stuck with no repair path, even after
// it is reparented under a healthy ancestor, because the freeze has no way to
// notice the parent got better. Clamping the degenerate axes to a small
// non-zero scale is what game engines do and it is RECOVERABLE: the frame the
// parent stops being singular, the child is correct again.
//
// A zero scale is not exotic — it is how content hides things, and how a
// spring or a shrink animation passes through zero on its way somewhere else.
// Returns false when the clamp was needed, so a caller that cares can say so.
// How many times a singular parent had to be clamped. A counter and not a log
// line per call: this sits in a per-frame path, and a parent that is singular
// this frame is singular every frame until the content changes.
inline uint64_t g_singularParentClamps = 0;

inline bool worldToLocalMatrix(float outLocal[16], const float world[16],
                               const float parentWorld[16]) {
    float parentInv[16];
    if (safeInvert(parentInv, parentWorld)) {
        bx::mtxMul(outLocal, world, parentInv);
        return true;
    }

    // Singular. Rebuild the parent with every degenerate axis clamped away
    // from zero, KEEPING ITS SIGN so a mirrored parent stays mirrored, and
    // invert that instead.
    bx::Vec3 p{0,0,0}; bx::Quaternion r{0,0,0,1}; bx::Vec3 s{1,1,1};
    decomposeMatrix(parentWorld, p, r, s);
    constexpr float kMinScale = 1e-4f;
    auto clamp = [](float v) {
        if (v > kMinScale || v < -kMinScale) return v;
        return v < 0.0f ? -kMinScale : kMinScale;   // +0.0 and -0.0 both go +
    };
    s = { clamp(s.x), clamp(s.y), clamp(s.z) };

    // Scale then rotate then translate, through the QUATERNION rather than
    // bx::mtxSRT's Euler angles, so a rotated parent survives the repair
    // unchanged instead of going through a conversion that need not round-trip.
    float rot[16]; bx::mtxFromQuaternion(rot, r);
    float scl[16]; bx::mtxScale(scl, s.x, s.y, s.z);
    float fixed[16]; bx::mtxMul(fixed, scl, rot);
    fixed[12] = p.x; fixed[13] = p.y; fixed[14] = p.z;

    ++g_singularParentClamps;
    if (g_singularParentClamps == 1)
        LOG_WARN("Transform", "a parent transform is singular (a zero or "
                 "near-zero scale); its children's local poses are computed "
                 "against a clamped copy rather than against identity, which "
                 "is what used to happen and moved them by the parent's whole "
                 "pose. Warned once; g_singularParentClamps counts the rest.");

    if (!safeInvert(parentInv, fixed)) {
        // Should be unreachable — every axis is now at least kMinScale — so if
        // it happens the parent matrix is not an SRT at all (shear, NaN) and
        // there is no local pose to compute. Leave the caller's value alone
        // rather than inventing one.
        return false;
    }
    bx::mtxMul(outLocal, world, parentInv);
    return false;
}

// Same, decomposed. `outScale` is optional — the physics write-back keeps the
// entity's authored scale rather than adopting the one it derives.
inline bool worldToLocalPose(bx::Vec3& outPos, bx::Quaternion& outRot,
                             bx::Vec3* outScale,
                             const float world[16], const float parentWorld[16]) {
    float local[16];
    const bool ok = worldToLocalMatrix(local, world, parentWorld);
    bx::Vec3 s{1,1,1};
    decomposeMatrix(local, outPos, outRot, s);
    if (outScale) *outScale = s;
    return ok;
}
