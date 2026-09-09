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
#include "core/logger.h"
#include "components/prev_transform.h"

// ── Hierarchy depth guard ────────────────────────────────────────────────────
// flecs HARD-ABORTS the process on a ChildOf chain deeper than
// FLECS_DAG_DEPTH_MAX (128) — it caches per-entity depth for query ordering.
// So an over-deep scene/script hierarchy would crash the engine. Keep engine-
// created parenting below a margin of that, and refuse (not abort) past it.
constexpr int kMaxHierarchyDepth = 120;

// Depth of e in the ChildOf tree (0 = root); bounded so a stray cycle can't spin.
inline int hierarchyDepth(flecs::entity e) {
    int d = 0;
    for (flecs::entity a = e.target(flecs::ChildOf);
         a && a.is_alive() && d < 4096; a = a.target(flecs::ChildOf)) ++d;
    return d;
}

// Reparent child under parent, refusing CYCLES (child would be its own ancestor
// → getWorldMatrix infinite-recurses) and OVER-DEEP chains (flecs would abort).
// Returns false with a logged reason instead of crashing. The single safe entry
// point for every engine parenting site.
inline bool safeReparent(flecs::entity child, flecs::entity parent) {
    if (!child.is_alive() || !parent.is_alive() || child == parent) return false;
    for (flecs::entity a = parent; a && a.is_alive(); a = a.target(flecs::ChildOf))
        if (a == child) { LOG_WARN("Scene", "reparent refused — would create a cycle"); return false; }
    if (hierarchyDepth(parent) + 1 >= kMaxHierarchyDepth) {
        LOG_WARN("Scene", "reparent refused — hierarchy deeper than %d (flecs would abort)",
                 kMaxHierarchyDepth);
        return false;
    }
    child.remove(flecs::ChildOf, flecs::Wildcard);
    child.add(flecs::ChildOf, parent);
    return true;
}

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

// The interpolated LOCAL matrix from components already in hand. `prev` may be
// null (no PrevTransform, or alpha == 1). Takes both components rather than an
// entity on purpose: a caller iterating a query has them, and looking either one
// up again is a per-entity sparse-set probe — see the measurements on
// getWorldMatrixLerpFrom.
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

inline void getWorldMatrixLerp(flecs::entity e, float alpha, float out[16],
                               int depth = 0) {
    float local[16];
    const Transform* t = e.try_get<Transform>();
    if (!t) bx::mtxIdentity(local);
    else    localMatrixLerp(*t, alpha < 1.0f ? e.try_get<PrevTransform>() : nullptr,
                            alpha, local);
    flecs::entity parent = e.target(flecs::ChildOf);
    if (parent && parent.is_alive() && parent.has<Transform>() && depth < 256) {
        float parentWorld[16];
        getWorldMatrixLerp(parent, alpha, parentWorld, depth + 1);
        bx::mtxMul(out, local, parentWorld);
    } else {
        std::memcpy(out, local, 16 * sizeof(float));
    }
}

// ── getWorldMatrixLerpFrom ─────────────────────────────────────────────────
// getWorldMatrixLerp for the one caller shape that dominates: a query iteration
// that ALREADY holds the entity's Transform. Identical result; it just does not
// look the component up a second time.
//
// MEASURED, by differential runs over a 20 000-entity scene, because the shape
// of the cost is not what reading the code suggests. getWorldMatrixLerp was
// 10.0 ms of a 15.2 ms extraction pass, and it is FOUR separate costs:
//
//   ~1.5 ms  try_get<Transform>, redundant — the query already had it
//   ~2.2 ms  target(ChildOf) + is_alive() + has<Transform>(), three pair lookups
//            to answer "no parent" for almost every entity
//   ~6.3 ms  try_get<PrevTransform> plus the interpolation itself
//   remainder the matrix compose
//
// None of it is arithmetic worth optimising; it is per-entity component lookups.
// And a function handed only an entity CANNOT avoid them — only the caller can,
// by letting the query engine answer per archetype instead of per entity. That
// is what Renderer::buildView does: it partitions on ChildOf with two queries and
// takes PrevTransform as an optional term. This overload is for the parented set,
// where the ancestor walk is genuinely needed.
inline void getWorldMatrixLerpFrom(flecs::entity e, const Transform& t,
                                   const PrevTransform* prev, float alpha,
                                   float out[16]) {
    float local[16];
    localMatrixLerp(t, prev, alpha, local);
    // Only ask about a parent if the entity actually has one. `parent()` is the
    // same ChildOf target lookup, but asking once is cheaper than the
    // target + is_alive + has<Transform> trio for a flat entity.
    flecs::entity parent = e.target(flecs::ChildOf);
    if (parent && parent.is_alive() && parent.has<Transform>()) {
        float parentWorld[16];
        getWorldMatrixLerp(parent, alpha, parentWorld, 1);
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
