// ── extract_partition_test — the two-query partition, and the matrices ───────
//
// Renderer extraction stopped using one query over renderables. It uses TWO,
// partitioned on whether the entity has a ChildOf parent, because asking each
// entity "do you have a parent?" cost 2.2 ms of a 15.2 ms extraction pass over
// 20 000 objects (see src/render/renderer/extract.cpp for the full breakdown).
//
// That optimisation has exactly one way to be wrong, and it is silent: if the two
// queries do not partition the set EXACTLY, entities vanish from the frame or get
// drawn twice, and nothing reports it. Both failures look like a content bug, not
// a renderer bug. So the partition is asserted here rather than assumed from
// reading flecs' `with`/`without` semantics.
//
// The second half asserts what the partition is allowed to change: nothing. A
// parentless entity's local matrix must equal what the full walk produced, and a
// parented one must still include every ancestor — including through an ancestor
// that has no Transform, and with PrevTransform interpolation active, which is
// where the fast path skips the most work.
//
// No renderer, no bgfx, no window: this is flecs plus core/transform_utils.h.
//
// SCOPE, stated precisely, because the gap matters. The matrix assertions exercise
// PRODUCTION code — localMatrixLerp and getWorldMatrixLerpFrom are the functions
// extraction calls. The partition assertions do NOT: they build their own queries
// with the same `with`/`without` terms, so they prove the TECHNIQUE is sound and
// would catch a flecs upgrade changing those semantics, but they cannot catch
// Renderer::init being edited to use the wrong term. Closing that would need the
// queries to come from somewhere reachable without a GPU device, which is a
// bigger seam than this optimisation justified. The engine-level evidence is that
// every submit counter is byte-identical at 1 k, 5 k and 20 k objects.
#include <cstdio>
#include <cmath>
#include <vector>

#include <flecs.h>
#include <bx/math.h>

#include "core/transform.h"
#include "core/transform_utils.h"
#include "components/prev_transform.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// A stand-in for MeshRenderer: this test is about the PARTITION, so it must not
// depend on the render component's contents (or drag bgfx in through mesh.h).
struct Renderable { int id = 0; };

static bool mtxNear(const float a[16], const float b[16], float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a[i] - b[i]) > eps) return false;
    return true;
}

static Transform xform(float x, float y, float z, float s = 1.0f) {
    Transform t;
    t.position = { x, y, z };
    t.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    t.scale    = { s, s, s };
    return t;
}

// ── 1. The partition is exact ───────────────────────────────────────────────
static void testPartitionIsExact() {
    std::printf("\n-- the two queries partition renderables exactly --\n");
    flecs::world w;

    // 6 parentless, 4 children, and one child-of-a-child, so the parented set
    // includes a depth-2 entity rather than only direct children.
    std::vector<flecs::entity> roots;
    for (int i = 0; i < 6; ++i)
        roots.push_back(w.entity().set<Transform>(xform((float)i, 0, 0))
                                  .set<Renderable>({ i }));
    for (int i = 0; i < 4; ++i)
        w.entity().child_of(roots[i]).set<Transform>(xform(0, 1, 0))
                  .set<Renderable>({ 100 + i });
    // A grandchild: parented, but its parent is itself parented.
    flecs::entity mid = w.entity().child_of(roots[5]).set<Transform>(xform(0, 2, 0));
    w.entity().child_of(mid).set<Transform>(xform(0, 0, 3))
              .set<Renderable>({ 200 });

    // An entity with a Transform but NO Renderable must appear in neither.
    w.entity().set<Transform>(xform(9, 9, 9));

    auto flat = w.query_builder<const Transform, const Renderable>()
                    .without(flecs::ChildOf, flecs::Wildcard).build();
    auto child = w.query_builder<const Transform, const Renderable>()
                    .with(flecs::ChildOf, flecs::Wildcard).build();
    auto all = w.query_builder<const Transform, const Renderable>().build();

    std::vector<uint64_t> seen;
    int nFlat = 0, nChild = 0, nAll = 0;
    flat.each([&](flecs::entity e, const Transform&, const Renderable&) {
        ++nFlat; seen.push_back(e.id());
        CHECK(!e.parent().is_valid(),
              "parentless query yielded an entity with no parent (id %llu)",
              (unsigned long long)e.id());
    });
    child.each([&](flecs::entity e, const Transform&, const Renderable&) {
        ++nChild; seen.push_back(e.id());
    });
    all.each([&](flecs::entity, const Transform&, const Renderable&) { ++nAll; });

    CHECK(nFlat == 6, "6 parentless renderables (%d)", nFlat);
    CHECK(nChild == 5, "5 parented renderables, incl. the grandchild (%d)", nChild);
    CHECK(nFlat + nChild == nAll,
          "together they cover the single query EXACTLY: %d + %d == %d",
          nFlat, nChild, nAll);

    // No entity in both halves — the double-draw failure.
    std::vector<uint64_t> sorted = seen;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
          "and no entity appears in both halves");
}

// ── 2. A parentless entity's fast path matches the full walk ────────────────
static void testFlatMatchesFullWalk() {
    std::printf("\n-- parentless: localMatrixLerp == getWorldMatrixLerp --\n");
    flecs::world w;
    Transform t = xform(3.0f, -2.0f, 7.0f, 2.0f);
    // A real rotation, so an error in the compose path cannot hide in identity.
    t.rotation = bx::fromEuler(bx::Vec3{ 0.3f, -0.7f, 1.1f });
    flecs::entity e = w.entity().set<Transform>(t);

    float fast[16], full[16];
    localMatrixLerp(*e.try_get<Transform>(), nullptr, 1.0f, fast);
    getWorldMatrixLerp(e, 1.0f, full);
    CHECK(mtxNear(fast, full),
          "identical matrices for a parentless entity");
}

// ── 3. A parented entity still gets its whole chain ─────────────────────────
static void testChildIncludesAncestors() {
    std::printf("\n-- parented: the ancestor chain is still applied --\n");
    flecs::world w;
    flecs::entity root  = w.entity().set<Transform>(xform(10.0f, 0.0f, 0.0f));
    flecs::entity mid   = w.entity().child_of(root).set<Transform>(xform(0.0f, 5.0f, 0.0f));
    flecs::entity leaf  = w.entity().child_of(mid).set<Transform>(xform(0.0f, 0.0f, 2.0f));

    float viaFrom[16], viaEntity[16];
    getWorldMatrixLerpFrom(leaf, *leaf.try_get<Transform>(), nullptr, 1.0f, viaFrom);
    getWorldMatrixLerp(leaf, 1.0f, viaEntity);
    CHECK(mtxNear(viaFrom, viaEntity),
          "the ...From overload agrees with the entity-only version");
    // Translations compose: 10 from root, 5 from mid, 2 from leaf.
    CHECK(std::fabs(viaFrom[12] - 10.0f) < 1e-4f &&
          std::fabs(viaFrom[13] -  5.0f) < 1e-4f &&
          std::fabs(viaFrom[14] -  2.0f) < 1e-4f,
          "world position is (10, 5, 2) — every ancestor applied (%.2f, %.2f, %.2f)",
          viaFrom[12], viaFrom[13], viaFrom[14]);

    // An ancestor with NO Transform terminates the walk, and the ChildOf
    // partition must not change that: the entity is still in the PARENTED half,
    // and its matrix is still its own local.
    flecs::entity bare = w.entity();                       // no Transform
    flecs::entity under = w.entity().child_of(bare).set<Transform>(xform(4.0f, 0, 0));
    float m[16];
    getWorldMatrixLerpFrom(under, *under.try_get<Transform>(), nullptr, 1.0f, m);
    CHECK(std::fabs(m[12] - 4.0f) < 1e-4f,
          "a parent with no Transform contributes nothing (%.2f)", m[12]);
}

// ── 4. PrevTransform as a passed-in pointer == as a looked-up component ─────
// This is the one the optimisation actually changed: PrevTransform used to be
// fetched per entity inside the matrix helper, and is now an optional query term
// handed in. If the two disagreed, every interpolated frame would be subtly wrong
// — the kind of bug that reads as "animation feels off" and never as a defect.
static void testPrevTransformInterpolation() {
    std::printf("\n-- interpolation is identical whether prev is passed or fetched --\n");
    flecs::world w;
    Transform cur = xform(10.0f, 0.0f, 0.0f);
    PrevTransform prev;
    prev.position = { 0.0f, 0.0f, 0.0f };
    prev.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    prev.scale    = { 1.0f, 1.0f, 1.0f };
    flecs::entity e = w.entity().set<Transform>(cur).set<PrevTransform>(prev);

    const float alpha = 0.25f;
    float passed[16], fetched[16];
    localMatrixLerp(cur, e.try_get<PrevTransform>(), alpha, passed);
    getWorldMatrixLerp(e, alpha, fetched);
    CHECK(mtxNear(passed, fetched), "same matrix either way");
    CHECK(std::fabs(passed[12] - 2.5f) < 1e-4f,
          "and it really interpolated: x = 2.5 at alpha 0.25 (%.3f)", passed[12]);

    // alpha == 1 must ignore prev entirely — the editor's default, and the
    // reason a stale PrevTransform never shows up while not simulating.
    float atOne[16], noPrev[16];
    localMatrixLerp(cur, e.try_get<PrevTransform>(), 1.0f, atOne);
    localMatrixLerp(cur, nullptr, 1.0f, noPrev);
    CHECK(mtxNear(atOne, noPrev), "alpha == 1 ignores PrevTransform");

    // A null prev must behave as "not interpolating", not as "lerp from origin",
    // which would snap every non-simulated entity toward (0,0,0).
    float nullPrev[16];
    localMatrixLerp(cur, nullptr, 0.25f, nullPrev);
    CHECK(std::fabs(nullPrev[12] - 10.0f) < 1e-4f,
          "a null prev holds the current transform, not a lerp from the origin "
          "(%.2f)", nullPrev[12]);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("extract_partition_test: the two-query extraction partition\n");

    testPartitionIsExact();
    testFlatMatchesFullWalk();
    testChildIncludesAncestors();
    testPrevTransformInterpolation();

    if (g_failures) {
        std::printf("\nextract_partition_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nextract_partition_test: all checks passed\n");
    return 0;
}
