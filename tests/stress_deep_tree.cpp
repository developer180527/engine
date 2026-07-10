// ── stress_deep_tree — transform hierarchy depth torture ────────────────────
// VULNERABILITY FOUND: flecs hard-ABORTS the process on a ChildOf chain deeper
// than FLECS_DAG_DEPTH_MAX (128) — so a deep scene/script hierarchy would crash
// the whole engine. Fix: every engine parenting site now goes through
// safeReparent (transform_utils.h), which refuses over-deep (and cyclic)
// parenting with a logged warning instead of aborting.
//
// This test guards that: build up to the safe limit (transforms must fully
// accumulate — no silent clipping), then confirm one link past it is refused
// gracefully rather than crashing.
#include <cstdio>

#include <flecs.h>

#include "core/transform.h"
#include "core/transform_utils.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                        \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("\n"); ++g_failures; }                \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    std::printf("stress_deep_tree: transform-chain depth (flecs abort guard)\n");

    flecs::world w;
    Transform step;                       // +1 X per link
    step.position = {1.0f, 0.0f, 0.0f};

    // Grow the chain link by link through the guarded path until it refuses.
    flecs::entity e = w.entity().set<Transform>(step);   // root: depth 0, world X=1
    int added = 0;
    bool refusedGracefully = false;
    for (int i = 0; i < 500; ++i) {
        flecs::entity child = w.entity().set<Transform>(step);
        if (!safeReparent(child, e)) { refusedGracefully = true; break; }
        e = child;
        ++added;
    }

    CHECK(refusedGracefully,
          "over-deep reparent REFUSED (no flecs abort) — engine survived");
    CHECK(added >= kMaxHierarchyDepth - 2 && added < 128,
          "grew to the safe limit: depth %d (guard %d, flecs abort 128)",
          added, kMaxHierarchyDepth);

    // The deepest entity's world transform must include EVERY ancestor — a
    // silent depth cap would clip it. Root X=1, each link +1 → X = added+1.
    float m[16];
    getWorldMatrix(e, m);
    CHECK(m[12] == (float)(added + 1),
          "getWorldMatrix accumulated the FULL chain: world X = %.0f (want %d)",
          m[12], added + 1);

    if (g_failures) { std::printf("stress_deep_tree: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_deep_tree: PASS — deep hierarchies guarded, not crashing\n");
    return 0;
}
