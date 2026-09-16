#pragma once
// TODO (Jun 4, 09:00 PM):
// Replace recursive world-transform computation with a cached transform system.
// Maintain LocalTransform, WorldTransform and dirty propagation.
// Avoid recomputing parent chains during rendering and editor operations.

// ── transform_hierarchy — world poses through the ChildOf chain ─────────────
//
// Split out of core/transform_utils.h (2026-09-16). These are exactly the
// functions that take a `flecs::entity`, and `src/core` may not include the
// ECS: src/core/info.md says that layer "should compile in a unit test with no
// GPU, no ECS, no filesystem", and the header broke its own rule by including
// <flecs.h> for the walkers below. The matrix math they call stayed in core —
// it never needed the ECS — and this is the half that genuinely does, because
// a parent chain IS an ECS relationship.
//
// ── WHY THIS LIVES IN components/ AND NOT scene/ ───────────────────────────
// Every caller already depends on components/: renderer extraction, the Jolt
// plugin, script_host, the editor panels, the scene serializer. So this home
// adds NO new module edge. scene/ would have created `render -> scene` and
// `plugins -> scene`, two new edges bought for a filing preference — which is
// the trade scripts/engine_audit.py's LAYER-03 rule exists to put in front of
// you before you make it, rather than after.
//
// These are helpers over the component structs next door; they are NOT part of
// the frozen kit ABI and nothing here is in componentLayoutHash().
#include <flecs.h>
#include <bx/math.h>
#include <cstring>

#include "core/transform.h"
#include "core/transform_utils.h"      // the matrix math half
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
// safe as the universal extraction path. The per-local interpolation itself is
// localMatrixLerp, in core/transform_utils.h.
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
