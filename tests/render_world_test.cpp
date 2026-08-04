// ── render_world_test — culling, sort keys, visibility, light packing ───────
//
// Phase 3 of docs/plans/renderer-audit-and-plan.md: the renderer's MACHINERY, split
// out of ForwardPipeline into src/render/world so it can be tested at all.
//
// Everything asserted here used to live inside a 442-line pipeline class that
// needs a GPU to instantiate — which is why culling and sort order, two things
// that fail SILENTLY (geometry pops out at the screen edge; batching quietly
// collapses into one draw per object), have never had a test that can fail.
//
// The whole point of RenderItem carrying its own bounds is that this file
// links without bgfx, without Mesh, and without a device.
#include <cstdio>
#include <thread>
#include <vector>
#include <cstring>
#include <vector>

#include "render/world/frustum.h"
#include "render/world/light_packing.h"
#include "render/world/sort_key.h"
#include "render/world/visibility.h"
#include "render/world/draw_sort.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace rworld;

// A dummy non-null Mesh pointer. Never dereferenced — that is the invariant
// being tested as much as anything else: if visibility ever starts reading
// through this, the test segfaults rather than quietly passing.
static const Mesh* kMesh = reinterpret_cast<const Mesh*>(0x1);

static Mat4 translation(float x, float y, float z) {
    Mat4 m;                        // identity
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

// A frustum that keeps everything with x/y/z in [-10,10]: an axis-aligned box
// standing in for a view. Planes point INWARD, normalized, matching what the
// engine hands the pipeline.
static void boxFrustum(float planes[6][4], float half = 10.0f) {
    const float n[6][3] = {{ 1,0,0},{-1,0,0},{0, 1,0},{0,-1,0},{0,0, 1},{0,0,-1}};
    for (int i = 0; i < 6; ++i) {
        planes[i][0] = n[i][0]; planes[i][1] = n[i][1]; planes[i][2] = n[i][2];
        planes[i][3] = half;    // inside => dot(n,p) + half > 0
    }
}

static RenderItem itemAt(float x, float y, float z, uint32_t mat, uint32_t mesh,
                         float size = 1.0f) {
    RenderItem it;
    it.model   = translation(x, y, z);
    it.mesh    = kMesh;
    it.matKey  = mat;
    it.meshKey = mesh;
    it.boundsCenter = Vec3{ 0.0f, 0.0f, 0.0f };
    it.boundsSize   = Vec3{ size, size, size };
    it.hasBounds    = true;
    return it;
}

int main() {
    std::printf("render_world_test\n");

    // ── frustum ─────────────────────────────────────────────────────────────
    {
        float planes[6][4];
        boxFrustum(planes);

        const Mat4 origin = translation(0, 0, 0);
        const BoundingSphere in =
            worldSphere(origin.m, Vec3{0,0,0}, Vec3{1,1,1});
        CHECK(!outsideFrustum(in, planes), "a sphere at the origin is visible");

        const Mat4 far = translation(100, 0, 0);
        const BoundingSphere out =
            worldSphere(far.m, Vec3{0,0,0}, Vec3{1,1,1});
        CHECK(outsideFrustum(out, planes), "a sphere at x=100 is culled");

        // The conservative direction. A sphere whose CENTRE is outside but
        // whose radius still crosses the plane must be KEPT: dropping it is
        // how geometry pops out of existence at the screen edge.
        const Mat4 edge = translation(0, 0, -10.4f);
        const BoundingSphere straddle =
            worldSphere(edge.m, Vec3{0,0,0}, Vec3{2,2,2});   // radius ~1.73
        CHECK(!outsideFrustum(straddle, planes),
              "a sphere straddling a plane is kept (conservative)");

        // Scale must reach the radius. Same local bounds, scaled 20x: the
        // object now spans the frustum and cannot be culled. This is the bug
        // that a naive worldSphere (ignoring the 3x3) would produce.
        Mat4 big = translation(0, 0, -14.0f);
        big.m[0] = big.m[5] = big.m[10] = 20.0f;
        const BoundingSphere scaled =
            worldSphere(big.m, Vec3{0,0,0}, Vec3{1,1,1});
        CHECK(scaled.radius > 10.0f, "model scale reaches the radius (%.2f)",
              scaled.radius);
        CHECK(!outsideFrustum(scaled, planes),
              "a 20x-scaled object at the edge is not culled");

        // localCenter is an OFFSET in local space, and must be transformed —
        // not added to the world position after the fact.
        const BoundingSphere offset =
            worldSphere(origin.m, Vec3{5,0,0}, Vec3{1,1,1});
        CHECK(offset.x > 4.9f && offset.x < 5.1f,
              "local centre offset lands in world space (x=%.2f)", offset.x);
    }

    // ── sort keys ───────────────────────────────────────────────────────────
    {
        // Blend class dominates everything. This is correctness, not tuning:
        // an opaque draw must precede a transparent one however near or far.
        const uint64_t opaqueFar =
            makeSortKey(BlendClass::Opaque, 1.0f, 0xFFFF, 0x1FFFFF);
        const uint64_t transparentNear =
            makeSortKey(BlendClass::Transparent, 0.0f, 0, 0);
        CHECK(opaqueFar < transparentNear,
              "the furthest opaque draw still sorts before the nearest "
              "transparent one");

        // Opaque: material outranks depth (preserves the batching the
        // pipeline had before this split — see sort_key.h).
        const uint64_t matA = makeSortKey(BlendClass::Opaque, 0.9f, 1, 5);
        const uint64_t matB = makeSortKey(BlendClass::Opaque, 0.1f, 2, 5);
        CHECK(matA < matB, "opaque: material outranks depth");

        // ...and depth is still the tiebreaker inside one material+mesh.
        const uint64_t near = makeSortKey(BlendClass::Opaque, 0.1f, 7, 3);
        const uint64_t away = makeSortKey(BlendClass::Opaque, 0.9f, 7, 3);
        CHECK(near < away, "opaque: near before far within one batch");

        // Transparent: depth dominates, FAR first, or blending is wrong.
        const uint64_t tFar  = makeSortKey(BlendClass::Transparent, 0.9f, 9, 9);
        const uint64_t tNear = makeSortKey(BlendClass::Transparent, 0.1f, 1, 1);
        CHECK(tFar < tNear, "transparent: far before near");

        // Accessors must decode both layouts, or a pass reading the key back
        // binds the wrong material for transparent draws.
        CHECK(materialOf(matB) == 2 && meshOf(matB) == 5,
              "opaque key decodes (mat=%u mesh=%u)", materialOf(matB),
              meshOf(matB));
        CHECK(materialOf(tFar) == 9 && meshOf(tFar) == 9,
              "transparent key decodes (mat=%u mesh=%u)", materialOf(tFar),
              meshOf(tFar));
        CHECK(blendOf(tFar) == BlendClass::Transparent, "blend class decodes");

        CHECK(sameBatch(near, away),
              "same material+mesh at different depths is one batch");
        CHECK(!sameBatch(matA, matB), "different materials are not one batch");
        // Transparent draws are depth-ordered and must never be collapsed.
        CHECK(!sameBatch(tFar, tFar),
              "transparent draws never batch, even against themselves");

        // Out-of-range depth must clamp rather than wrap into the material
        // field — a NaN/overflowed depth reordering the whole frame is the
        // kind of bug that only shows up as flicker.
        const uint64_t lo = makeSortKey(BlendClass::Opaque, -5.0f, 4, 4);
        const uint64_t hi = makeSortKey(BlendClass::Opaque,  5.0f, 4, 4);
        CHECK(materialOf(lo) == 4 && materialOf(hi) == 4,
              "out-of-range depth clamps, material survives");
        CHECK(lo < hi, "clamped depths still order correctly");
    }

    // ── visibility ──────────────────────────────────────────────────────────
    {
        ViewCamera cam;
        boxFrustum(cam.frustum);
        cam.camPos = Vec4{ 0.0f, 0.0f, 0.0f, 1.0f };

        std::vector<RenderItem> items;
        items.push_back(itemAt(0,   0, -2, /*mat*/1, /*mesh*/1));  // visible
        items.push_back(itemAt(0,   0, -8, /*mat*/1, /*mesh*/1));  // visible, far
        items.push_back(itemAt(500, 0,  0, /*mat*/1, /*mesh*/1));  // culled
        items.push_back(itemAt(0,   0, -4, /*mat*/2, /*mesh*/1));  // visible

        RenderWorld world;
        world.items = Span<RenderItem>{ items.data(), items.size() };

        VisibleSet set;
        buildVisibleSet(world, cam, set);

        CHECK(set.consideredCount == 4, "considered every item (%u)",
              set.consideredCount);
        CHECK(set.culledCount == 1, "culled exactly the off-screen one (%u)",
              set.culledCount);
        CHECK(set.size() == 3, "3 draws survive (%zu)", set.size());

        // Sorted ascending, and grouped by material — the two mat=1 items
        // adjacent, mat=2 after.
        bool ascending = true;
        for (std::size_t i = 1; i < set.draws.size(); ++i)
            if (set.draws[i].key < set.draws[i-1].key) ascending = false;
        CHECK(ascending, "draws come out sorted");
        CHECK(materialOf(set.draws[0].key) == 1
              && materialOf(set.draws[1].key) == 1
              && materialOf(set.draws[2].key) == 2,
              "material 1 batched ahead of material 2");
        CHECK(batchRunLength(set, 0) == 2, "the mat=1 run is 2 draws (%zu)",
              batchRunLength(set, 0));
        CHECK(batchRunLength(set, 2) == 1, "the mat=2 run is 1 draw");
        CHECK(batchRunLength(set, 99) == 0, "out-of-range run length is 0");

        // Within the batch, the nearer of the two mat=1 items comes first.
        CHECK(set.draws[0].index == 0 && set.draws[1].index == 1,
              "near-first inside the batch (%u, %u)", set.draws[0].index,
              set.draws[1].index);

        // An item with no mesh is not a draw.
        RenderItem noMesh = itemAt(0, 0, -1, 3, 3);
        noMesh.mesh = nullptr;
        items.push_back(noMesh);
        // An item with no BOUNDS is never culled — missing data must look like
        // an asset problem, not a rendering one.
        RenderItem unbounded = itemAt(9999, 0, 0, 4, 4);
        unbounded.hasBounds = false;
        items.push_back(unbounded);

        world.items = Span<RenderItem>{ items.data(), items.size() };
        buildVisibleSet(world, cam, set);
        CHECK(set.size() == 4, "meshless item dropped, unbounded item kept "
              "(%zu draws)", set.size());
        bool sawUnbounded = false;
        for (const auto& d : set.draws)
            if (materialOf(d.key) == 4) sawUnbounded = true;
        CHECK(sawUnbounded, "the unbounded item survives at x=9999");

        // Reuse must reset. The set is deliberately kept across frames for its
        // capacity; a stale draw list is a use-after-free waiting to happen.
        RenderWorld empty;
        buildVisibleSet(empty, cam, set);
        CHECK(set.empty() && set.consideredCount == 0 && set.culledCount == 0,
              "an empty world clears the reused set");
    }

    // ── the cull is SCHEDULING-INDEPENDENT ──────────────────────────────────
    // buildVisibleSet takes an injected ParallelForFn so it can run its per-item
    // test on the engine's job pool without this layer depending on the runtime.
    // The risk that buys is a result that varies with how the work was split —
    // ranges finishing out of order, an entity landing in the wrong output slot,
    // a reduction (maxDist, culledCount) losing an update. maxDist is the nastiest
    // of those: it normalises the whole frame's depth, so losing one range's
    // contribution reorders EVERY draw, and nothing would crash.
    //
    // So: run the same world four ways and demand byte-identical draw lists. The
    // dispatchers are deliberately hostile — reverse order, one range at a time,
    // and real threads.
    {
        std::printf("\n-- cull: identical results however the work is scheduled --\n");

        // A generous box frustum so the "visible" items below really are inside
        // it; the culled ones sit far outside on purpose.
        ViewCamera cam;
        boxFrustum(cam.frustum, 100.0f);
        cam.camPos = Vec4{ 0.0f, 0.0f, 0.0f, 1.0f };

        // Enough items to exceed kCullParallelMin (4096) and span several
        // ranges, with a mix that culls: half in front of the camera, half far
        // behind it, and varied materials/meshes so the sort has real work.
        std::vector<RenderItem> items;
        items.reserve(9000);
        // Distance INCREASES with index on purpose, so the ranges do not all see
        // the same depth spread. With `i % 90` every range had nearly the same
        // local maximum, which made a lost maxDist reduction undetectable no
        // matter how the ranges were scheduled — the test passed a mutation.
        for (int i = 0; i < 9000; ++i) {
            const bool visible = (i % 3) != 0;
            const float x = (float)((i % 40) - 20) * 0.5f;
            const float z = visible ? -(1.0f + (float)i * 0.01f) : 900.0f;
            items.push_back(itemAt(x, 0.0f, z, (uint32_t)(i % 7),
                                   (uint32_t)(i % 11)));
        }
        // A meshless and an unbounded item, so the gap-closing path is exercised
        // inside a range rather than only at range boundaries.
        items[100].mesh = nullptr;
        items[5000].hasBounds = false;

        RenderWorld world;
        world.items = Span<RenderItem>{ items.data(), items.size() };

        VisibleSet ref;
        buildVisibleSet(world, cam, ref, nullptr);          // serial baseline
        CHECK(ref.size() > 1000 && ref.culledCount > 1000,
              "the baseline both keeps and culls a lot (%zu draws, %u culled)",
              ref.size(), ref.culledCount);

        // AN ABSOLUTE CHECK, not a comparison. Everything below compares the
        // threaded runs against this serial one, which cannot catch a bug the two
        // share — and the per-range maxDist reduction IS shared. If it loses a
        // range's contribution the normaliser is too small, and every draw past it
        // clamps to the same saturated depth code: depth ordering inside a material
        // group is gone, silently, in both paths. Only the FARTHEST draw may
        // saturate.
        {
            uint32_t saturated = 0;
            for (const auto& d : ref.draws)
                if (depthOf(d.key) == 0xFFFFFFu) ++saturated;
            CHECK(saturated <= 1,
                  "at most one draw sits at full-scale depth — the frame's depth "
                  "normaliser covers the whole range (%u saturated of %zu)",
                  saturated, ref.draws.size());
        }
        // And within one material+mesh group, draws run near-to-front first —
        // the documented tiebreaker behaviour of the opaque key layout.
        {
            bool monotonic = true;
            for (std::size_t i = 1; i < ref.draws.size(); ++i) {
                const uint64_t a = ref.draws[i - 1].key, b = ref.draws[i].key;
                if (materialOf(a) == materialOf(b) && meshOf(a) == meshOf(b)
                    && depthOf(a) > depthOf(b)) { monotonic = false; break; }
            }
            CHECK(monotonic,
                  "inside one material+mesh run, depth is non-decreasing");
        }

        auto sameAsRef = [&](const VisibleSet& other, const char* how) {
            bool ok = other.size() == ref.size()
                   && other.culledCount == ref.culledCount
                   && other.consideredCount == ref.consideredCount;
            if (ok)
                for (std::size_t i = 0; i < ref.draws.size(); ++i)
                    if (other.draws[i].key   != ref.draws[i].key
                     || other.draws[i].index != ref.draws[i].index) { ok = false; break; }
            CHECK(ok, "%s gives a byte-identical draw list (%zu vs %zu draws, "
                      "%u vs %u culled)", how, other.size(), ref.size(),
                      other.culledCount, ref.culledCount);
        };

        // 1. Sequential, but through the dispatcher — proves the threaded code
        //    path itself (chunking + compaction) matches the serial one.
        VisibleSet a;
        ParallelForFn seq = [](uint32_t count, uint32_t,
                               const std::function<void(uint32_t,uint32_t)>& fn) {
            for (uint32_t i = 0; i < count; ++i) fn(i, i + 1);
        };
        buildVisibleSet(world, cam, a, &seq);
        sameAsRef(a, "one range at a time");

        // 2. REVERSE order. If any range depended on an earlier one having run —
        //    a shared cursor, an append instead of an indexed write — this breaks.
        VisibleSet b;
        ParallelForFn rev = [](uint32_t count, uint32_t,
                               const std::function<void(uint32_t,uint32_t)>& fn) {
            for (uint32_t i = count; i-- > 0; ) fn(i, i + 1);
        };
        buildVisibleSet(world, cam, b, &rev);
        sameAsRef(b, "ranges executed in REVERSE");

        // 3. REAL THREADS, one per range. The only check here that can catch an
        //    actual data race rather than an ordering assumption.
        VisibleSet c;
        ParallelForFn threaded = [](uint32_t count, uint32_t,
                                    const std::function<void(uint32_t,uint32_t)>& fn) {
            std::vector<std::thread> ts;
            ts.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                ts.emplace_back([&fn, i] { fn(i, i + 1); });
            for (auto& t : ts) t.join();      // BLOCKS, as the contract requires
        };
        buildVisibleSet(world, cam, c, &threaded);
        sameAsRef(c, "one OS thread per range");

        // 4. Reused set, threaded, twice — the capacity-reuse path under threads.
        buildVisibleSet(world, cam, c, &threaded);
        sameAsRef(c, "a reused set, run threaded twice");
    }

    // ── sortDraws: it replaced std::sort, so it answers for itself ──────────
    // A radix sort is only worth having if it is provably a sort. Three things
    // are asserted, in order of how badly they would break rendering:
    //   * ORDER — a wrong order draws the scene in the wrong sequence and, for
    //     the opaque key layout, silently destroys batching;
    //   * PERMUTATION — a radix pass that drops or duplicates an element loses
    //     draws or draws them twice, and the counters would still look sane;
    //   * STABILITY — this is a behaviour CHANGE from std::sort, which is not
    //     stable. It is the reason "byte-identical submit counters" is now a
    //     meaningful claim, so it is pinned rather than left as an implementation
    //     accident.
    {
        std::printf("\n-- sortDraws: a stable radix sort over 64-bit keys --\n");

        auto checkSorted = [](const std::vector<VisibleDraw>& v, const char* how) {
            bool ok = true;
            for (std::size_t i = 1; i < v.size(); ++i)
                if (v[i - 1].key > v[i].key) { ok = false; break; }
            CHECK(ok, "%s comes out ascending by key (%zu draws)", how, v.size());
        };

        std::vector<VisibleDraw> scratch;

        // 1. Keys spanning every byte, so no pass is skipped. A deterministic LCG:
        //    a fixed seed means a failure is reproducible.
        {
            uint32_t seed = 0xC0FFEEu;
            auto rnd = [&seed] {
                seed = seed * 1664525u + 1013904223u;
                return seed;
            };
            std::vector<VisibleDraw> v;
            for (uint32_t i = 0; i < 5000; ++i)
                v.push_back({ ((uint64_t)rnd() << 32) | rnd(), i });
            const std::vector<VisibleDraw> before = v;
            sortDraws(v, scratch);
            checkSorted(v, "keys spread over all 8 bytes");
            CHECK(v.size() == before.size(), "no draws gained or lost (%zu)",
                  v.size());
            // A permutation, checked by summing indices — cheap, and catches the
            // duplicate-element failure a sortedness check happily accepts.
            uint64_t sumBefore = 0, sumAfter = 0;
            for (auto& d : before) sumBefore += d.index;
            for (auto& d : v)      sumAfter  += d.index;
            CHECK(sumBefore == sumAfter, "the result is a permutation of the input");
        }

        // 2. The byte-skipping path: only the low bytes vary, which is the shape a
        //    real frame has (one blend class, few materials, few meshes). If the
        //    skip logic were wrong this would come out unsorted or unstable.
        {
            std::vector<VisibleDraw> v;
            for (uint32_t i = 0; i < 4000; ++i) {
                const uint64_t high = 0x0000ABCDEF000000ull;   // constant
                v.push_back({ high | (uint64_t)((4000 - i) & 0xFFFFFFu), i });
            }
            sortDraws(v, scratch);
            checkSorted(v, "keys sharing every high byte");
            CHECK(v.front().index == 3999 && v.back().index == 0,
                  "and the reversed input is fully reversed back "
                  "(front %u, back %u)", v.front().index, v.back().index);
        }

        // 3. STABILITY. Every key identical, so the only correct answer is the
        //    input order preserved. std::sort would have been free to shuffle it.
        {
            std::vector<VisibleDraw> v;
            for (uint32_t i = 0; i < 1000; ++i)
                v.push_back({ 0x1234567800000000ull, i });
            sortDraws(v, scratch);
            bool stable = true;
            for (uint32_t i = 0; i < v.size(); ++i)
                if (v[i].index != i) { stable = false; break; }
            CHECK(stable, "1000 identical keys keep item order exactly");
        }
        // Ties AMONG varying keys, the realistic case: groups of equal keys must
        // each stay internally ordered.
        {
            std::vector<VisibleDraw> v;
            for (uint32_t i = 0; i < 3000; ++i)
                v.push_back({ (uint64_t)(i % 5) << 40, i });
            sortDraws(v, scratch);
            checkSorted(v, "5 key groups");
            bool stable = true;
            for (std::size_t i = 1; i < v.size(); ++i)
                if (v[i - 1].key == v[i].key && v[i - 1].index > v[i].index) {
                    stable = false; break;
                }
            CHECK(stable, "and within each group, item order is preserved");
        }

        // 4. Degenerate sizes — the early-out and the single-pass boundary.
        {
            std::vector<VisibleDraw> v;
            sortDraws(v, scratch);
            CHECK(v.empty(), "an empty list sorts to empty, without touching memory");
            v.push_back({ 42, 7 });
            sortDraws(v, scratch);
            CHECK(v.size() == 1 && v[0].index == 7, "a single draw survives intact");
            v.push_back({ 1, 9 });
            sortDraws(v, scratch);
            CHECK(v.size() == 2 && v[0].key == 1 && v[1].key == 42,
                  "two draws swap into order");
        }
    }

    // ── light packing ───────────────────────────────────────────────────────
    {
        // No lights => a default key light, not a black screen.
        const PackedLights def = packLights(Span<LightItem>{}, 0.1f);
        CHECK(def.count == 1, "empty light list synthesizes a key light");
        CHECK(def.shadowLightIndex == 0, "the default light casts the shadow");
        CHECK(def.ambient == 0.1f, "ambient passes through");
        CHECK(def.vec4Count() == 4, "one light is 4 vec4s");

        std::vector<LightItem> lights;
        LightItem a;                        // no shadows
        a.type = LightType::Point;
        a.position = Vec3{ 1.0f, 2.0f, 3.0f };
        a.intensity = 4.0f;
        lights.push_back(a);
        LightItem b;                        // the shadow caster
        b.type = LightType::Directional;
        b.castShadows = true;
        lights.push_back(b);

        const PackedLights p =
            packLights(Span<LightItem>{ lights.data(), lights.size() }, 0.2f);
        CHECK(p.count == 2, "both lights packed");
        CHECK(p.shadowLightIndex == 1, "the shadow caster is index 1 (%d)",
              p.shadowLightIndex);
        CHECK(p.data[0] == 1.0f && p.data[1] == 2.0f && p.data[2] == 3.0f,
              "position lands in slot 0.xyz");
        CHECK(p.data[3] == (float)(uint32_t)LightType::Point,
              "type rides in slot 0.w");
        CHECK(p.data[7] == 4.0f, "intensity rides in slot 1.w");
        CHECK(p.data[kFloatsPerLight + 3] == (float)(uint32_t)LightType::Directional,
              "the second light starts exactly one stride in");

        // Over the cap: extras are dropped, and the shadow index must refer to
        // the PACKED slot. Here the caster is source #20 but packed #0 is not
        // it — the index must be -1 rather than a lie.
        std::vector<LightItem> many(kMaxLights + 4);   // none cast shadows
        many.back().castShadows = true;                // beyond the cap
        const PackedLights capped =
            packLights(Span<LightItem>{ many.data(), many.size() }, 0.0f);
        CHECK(capped.count == kMaxLights, "packing stops at the cap (%d)",
              capped.count);
        CHECK(capped.shadowLightIndex == -1,
              "a shadow caster dropped at the cap does not leave a stale index "
              "(%d)", capped.shadowLightIndex);
    }

    if (g_failures) {
        std::printf("render_world_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("render_world_test: PASS\n");
    return 0;
}
