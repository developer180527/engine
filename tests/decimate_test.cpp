// ── decimate_test — the half of LOD that was missing ───────────────────────
//
// R20 built LOD selection and measured it at exactly zero benefit, for one
// reason: nothing could produce a cheaper mesh, so every "level" had the same
// triangle count as the one above it. Selection was correct and had nothing to
// select. This asserts the property that makes LOD worth having at all —
// **a level is genuinely cheaper** — plus the robustness a cooker needs when it
// runs unattended over content nobody inspects.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "assets/cookers/mesh/decimate.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace meshcook;

// A vertex with more than a position, so the test proves whole vertices are
// carried across — a decimator that only kept positions would pass a
// position-only fixture and ship meshes with no normals or UVs.
struct V { float px, py, pz; float nx, ny, nz; float u, v; };
static constexpr uint32_t kStride = sizeof(V);

// An NxN grid of quads on the XZ plane: a predictable triangle count, and
// coplanar so clustering has obvious work to do.
static void makeGrid(int n, std::vector<V>& verts, std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            verts.push_back({ (float)x, 0.0f, (float)z,
                              0.0f, 1.0f, 0.0f,
                              (float)x / n, (float)z / n });
    auto at = [&](int x, int z) { return (uint32_t)(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            idx.push_back(at(x, z));     idx.push_back(at(x + 1, z)); idx.push_back(at(x, z + 1));
            idx.push_back(at(x + 1, z)); idx.push_back(at(x + 1, z + 1)); idx.push_back(at(x, z + 1));
        }
}

static DecimateInput inputOf(const std::vector<V>& v, const std::vector<uint32_t>& i) {
    DecimateInput in;
    in.vertices    = (const uint8_t*)v.data();
    in.vertexCount = (uint32_t)v.size();
    in.stride      = kStride;
    in.posOffset   = 0;
    in.indices     = i.data();
    in.indexCount  = (uint32_t)i.size();
    return in;
}

int main() {
    std::printf("decimate_test\n");

    std::vector<V> verts; std::vector<uint32_t> idx;
    makeGrid(16, verts, idx);                    // 16x16 quads = 512 triangles
    const uint32_t baseTris = (uint32_t)idx.size() / 3;
    CHECK(baseTris == 512, "fixture is 512 triangles (%u)", baseTris);

    // ── THE POINT: a level is genuinely cheaper ─────────────────────────────
    {
        const auto r = decimate(inputOf(verts, idx), 8);
        CHECK(r.ok, "decimate succeeds (%s)", r.error);
        CHECK(r.triangles < baseTris, "coarser grid means FEWER triangles (%u < %u)",
              r.triangles, baseTris);
        CHECK((float)r.triangles <= kMinReductionRatio * baseTris,
              "and meaningfully fewer — under the %.0f%% ship threshold (%u)",
              kMinReductionRatio * 100.0f, r.triangles);
        CHECK(r.vertexCount(kStride) < (uint32_t)verts.size(),
              "vertices are compacted too, not just indices (%u < %zu)",
              r.vertexCount(kStride), verts.size());
    }

    // Monotonicity: coarser grid, fewer triangles. Without this a "level 3"
    // could be more expensive than level 2 and the chain would be nonsense.
    {
        uint32_t prev = baseTris + 1;
        bool monotone = true;
        for (uint32_t res : { 16u, 12u, 8u, 4u, 2u }) {
            const auto r = decimate(inputOf(verts, idx), res);
            if (!r.ok || r.triangles > prev) monotone = false;
            prev = r.triangles;
        }
        CHECK(monotone, "triangle count never increases as the grid coarsens");
        CHECK(prev < baseTris / 4, "the coarsest level is drastically cheaper (%u)", prev);
    }

    // ── whole vertices survive, not just positions ──────────────────────────
    {
        const auto r = decimate(inputOf(verts, idx), 8);
        bool attrsIntact = r.vertexCount(kStride) > 0;
        for (uint32_t i = 0; i < r.vertexCount(kStride); ++i) {
            V got{};
            std::memcpy(&got, r.vertices.data() + (size_t)i * kStride, kStride);
            // Every fixture vertex has this exact normal; a decimator that
            // averaged or dropped attributes would not.
            if (got.ny != 1.0f || got.nx != 0.0f || got.nz != 0.0f) attrsIntact = false;
            if (got.u < 0.0f || got.u > 1.0f) attrsIntact = false;
        }
        CHECK(attrsIntact, "normals and UVs come through intact — a surviving "
                           "vertex is a REAL vertex, never an average");
    }

    // ── indices stay in range ───────────────────────────────────────────────
    {
        const auto r = decimate(inputOf(verts, idx), 5);
        const uint32_t vc = r.vertexCount(kStride);
        bool inRange = true;
        for (uint32_t i : r.indices) if (i >= vc) inRange = false;
        CHECK(inRange, "every emitted index addresses a compacted vertex");
        CHECK(r.indices.size() % 3 == 0, "output is whole triangles");
    }

    // ── determinism: the DDC depends on it ──────────────────────────────────
    // Two machines must cook byte-identical levels or the cache stops being a
    // cache. Clustering uses a hash map, so this is a real risk, not a
    // formality — the representative is chosen by index order for this reason.
    {
        const auto a = decimate(inputOf(verts, idx), 7);
        const auto b = decimate(inputOf(verts, idx), 7);
        CHECK(a.indices == b.indices && a.vertices == b.vertices,
              "identical input yields byte-identical output");
    }

    // ── robustness: this runs unattended over content nobody inspects ───────
    {
        DecimateInput bad = inputOf(verts, idx);
        bad.vertices = nullptr;
        CHECK(!decimate(bad, 8).ok, "null vertices rejected, not dereferenced");

        bad = inputOf(verts, idx);
        bad.indexCount = 7;                    // not a multiple of 3
        CHECK(!decimate(bad, 8).ok, "a non-triangle index count is rejected");

        bad = inputOf(verts, idx);
        bad.stride = 8;                        // cannot hold a float3 at offset 0
        CHECK(!decimate(bad, 8).ok, "a stride too small for the position is rejected");

        CHECK(!decimate(inputOf(verts, idx), 0).ok, "grid resolution 0 is rejected");
    }
    {
        // Out-of-range indices are corrupt input, and cooked meshes travel
        // through a SHARED DDC — another machine's bytes. Drop the triangle,
        // don't read past the vertex buffer.
        std::vector<uint32_t> evil = idx;
        evil[0] = 999999;
        const auto r = decimate(inputOf(verts, evil), 8);
        CHECK(r.ok, "a corrupt index does not fail the whole decimation");
        const uint32_t vc = r.vertexCount(kStride);
        bool inRange = true;
        for (uint32_t i : r.indices) if (i >= vc) inRange = false;
        CHECK(inRange, "...and never produces an out-of-range index of its own");
    }
    {
        // Every vertex coincident: no grid to build. Must return the mesh
        // rather than divide by a zero extent.
        std::vector<V> flat(64, V{ 1, 1, 1, 0, 1, 0, 0, 0 });
        std::vector<uint32_t> fi;
        for (uint32_t i = 0; i + 2 < 64; i += 3) { fi.push_back(i); fi.push_back(i+1); fi.push_back(i+2); }
        const auto r = decimate(inputOf(flat, fi), 8);
        CHECK(r.ok, "a zero-extent mesh does not divide by zero");
        CHECK(r.triangles == (uint32_t)fi.size() / 3,
              "...and is returned unchanged rather than collapsed to nothing");
    }
    {
        std::vector<V> none; std::vector<uint32_t> ni;
        const auto r = decimate(inputOf(none, ni), 8);
        CHECK(r.ok && r.triangles == 0, "an empty mesh is an empty success");
    }

    // ── decimateToRatio: what an author actually specifies ──────────────────
    // A fixed grid is unusable across mixed content (96% reduction on a dense
    // mesh, 0% on a low-poly prop), so the cooker steers by triangle ratio.
    {
        for (float target : { 0.5f, 0.25f, 0.1f }) {
            const auto r = decimateToRatio(inputOf(verts, idx), target);
            const float got = (float)r.triangles / (float)baseTris;
            CHECK(r.ok && r.triangles > 0, "ratio %.2f produces a mesh", target);
            // Clustering is quantised, so the search lands NEAR the target, not
            // on it. What must hold is that it is under the parent and in the
            // right neighbourhood.
            CHECK(got < 1.0f && got <= target * 2.5f,
                  "...at %.1f%% for a %.0f%% target", got * 100.0f, target * 100.0f);
        }
        const auto a = decimateToRatio(inputOf(verts, idx), 0.25f);
        const auto b = decimateToRatio(inputOf(verts, idx), 0.25f);
        CHECK(a.indices == b.indices, "the search is deterministic too");

        CHECK(!decimateToRatio(inputOf(verts, idx), 0.0f).ok, "ratio 0 rejected");
        CHECK(!decimateToRatio(inputOf(verts, idx), 1.0f).ok, "ratio 1 rejected");
        CHECK(!decimateToRatio(inputOf(verts, idx), -1.0f).ok, "negative ratio rejected");
    }

    if (g_failures) {
        std::printf("decimate_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("decimate_test: PASS\n");
    return 0;
}
