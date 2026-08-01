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
#include <cstring>
#include <vector>

#include "render/world/frustum.h"
#include "render/world/light_packing.h"
#include "render/world/sort_key.h"
#include "render/world/visibility.h"

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
