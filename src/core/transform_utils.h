// TODO (Jun 4, 09:00 PM):
// Replace recursive world-transform computation with a cached transform system.
// Maintain LocalTransform, WorldTransform and dirty propagation.
// Avoid recomputing parent chains during rendering and editor operations.


#pragma once
#include <flecs.h>
#include <bx/math.h>
#include <cstring>
#include <cmath>
#include "core/transform.h"
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

// ── getWorldMatrix ─────────────────────────────────────────────────────────
// Walk the flecs ChildOf chain and accumulate transforms.
// Row-major: world = local * parent_world
inline void getWorldMatrix(flecs::entity e, float out[16], int depth = 0) {
    float local[16];
    if (const Transform* t = e.try_get<Transform>())
        t->getMatrix(local);
    else
        bx::mtxIdentity(local);

    flecs::entity parent = e.target(flecs::ChildOf);
    // depth cap is a backstop: a cycle that slips past the reparent/load
    // guards stops here instead of overflowing the stack.
    if (parent && parent.is_alive() && parent.has<Transform>() && depth < 256) {
        float parentWorld[16];
        getWorldMatrix(parent, parentWorld, depth + 1);
        bx::mtxMul(out, local, parentWorld);
    } else {
        std::memcpy(out, local, 16 * sizeof(float));
    }
}

// ── getWorldMatrixLerp ─────────────────────────────────────────────────────
// getWorldMatrix, but each local is nlerp(PrevTransform, Transform, alpha) —
// the render-side of the fixed-timestep loop. Entities without PrevTransform
// (editor world, cameras, alpha==1) use their current transform, so this is
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

inline void getWorldMatrixLerp(flecs::entity e, float alpha, float out[16],
                               int depth = 0) {
    float local[16];
    const Transform* t = e.try_get<Transform>();
    if (!t) {
        bx::mtxIdentity(local);
    } else {
        const PrevTransform* p =
            alpha < 1.0f ? e.try_get<PrevTransform>() : nullptr;
        if (!p) {
            t->getMatrix(local);
        } else {
            Transform tmp = *t;
            tmp.position = bx::lerp(p->position, t->position, alpha);
            tmp.rotation = nlerpQuat(p->rotation, t->rotation, alpha);
            tmp.scale    = bx::lerp(p->scale, t->scale, alpha);
            tmp.getMatrix(local);
        }
    }
    flecs::entity parent = e.target(flecs::ChildOf);
    if (parent && parent.is_alive() && parent.has<Transform>() && depth < 256) {
        float parentWorld[16];
        getWorldMatrixLerp(parent, alpha, parentWorld, depth + 1);
        bx::mtxMul(out, local, parentWorld);
    } else {
        std::memcpy(out, local, 16 * sizeof(float));
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

// ── isAncestorOf ───────────────────────────────────────────────────────────
// True if `ancestor` appears on the ChildOf chain above `node`. Depth-capped
// so an already-cyclic graph can't hang the walk. Read-only: safe to call
// inside a flecs query iteration.
inline bool isAncestorOf(flecs::entity ancestor, flecs::entity node) {
    if (!ancestor.is_alive() || !node.is_alive()) return false;
    flecs::entity p = node.target(flecs::ChildOf);
    for (int guard = 0; p && p.is_alive() && guard < 4096; ++guard) {
        if (p == ancestor) return true;
        p = p.target(flecs::ChildOf);
    }
    return false;
}
