#pragma once
// ── Decimation — produce a genuinely cheaper mesh ───────────────────────────
//
// ONE concern: given a mesh, return one with fewer triangles.
//
// R20 built LOD selection — screen-height metric, mutation-tested, correct — and
// it bought NOTHING measurable, for exactly one reason: `MeshCooker` could not
// decimate, so the fuzz generator built chains out of other kit meshes and no
// level had fewer triangles than the one above it. Selection was verified;
// there was simply nothing cheaper to select. This is the missing half.
//
// ALGORITHM: VERTEX CLUSTERING. Positions are quantised to a grid, vertices
// sharing a cell merge into one, and triangles whose corners collapse into the
// same cell disappear. Chosen over quadric error metrics deliberately:
//
//   • it cannot fail or produce non-manifold output — there is no topology to
//     get wrong, which matters because this runs unattended in a cooker over
//     content nobody inspects;
//   • it is O(n) and deterministic, so two machines cook byte-identical levels
//     and the DDC stays a cache rather than a coin flip;
//   • the grid size is a direct, predictable quality knob.
//
// The cost is quality: QEM preserves silhouettes better at the same triangle
// budget, and `meshoptimizer` does it well. Worth revisiting once there is a
// measurement saying silhouette error is what hurts — see info.md. For now the
// question is whether LOD pays off at all, and that needs cheaper levels, not
// prettier ones.
#include <cstdint>
#include <vector>

namespace meshcook {

struct DecimateInput {
    const uint8_t* vertices     = nullptr;
    uint32_t       vertexCount  = 0;
    uint32_t       stride       = 0;   // bytes per vertex
    uint32_t       posOffset    = 0;   // byte offset of float3 position
    const uint32_t* indices     = nullptr;
    uint32_t       indexCount   = 0;   // must be a multiple of 3
};

struct DecimateResult {
    std::vector<uint8_t>  vertices;    // compacted: only surviving vertices
    std::vector<uint32_t> indices;
    uint32_t triangles = 0;
    bool ok = false;
    const char* error = "";

    uint32_t vertexCount(uint32_t stride) const {
        return stride ? (uint32_t)(vertices.size() / stride) : 0;
    }
};

// `gridResolution` is the number of cells along the LONGEST bounding-box axis.
// Lower = coarser. A vertex survives as the representative of its cell.
//
// The representative is the cell's FIRST vertex in index order, not its
// centroid: a centroid drifts off the surface on thin geometry, and averaging
// normals/UVs/skin weights across a cell produces values that belong to no
// original vertex. Picking a real vertex keeps every attribute self-consistent,
// which matters because this copies the whole vertex, attributes included.
DecimateResult decimate(const DecimateInput& in, uint32_t gridResolution);

// Decimate toward a TRIANGLE RATIO (0.5 = about half), which is what an author
// actually wants to specify. Grid resolution is absolute — cells along the
// longest axis — so the same value gives 96% reduction on a dense mesh and 0% on
// a low-poly prop. Measured on a real 148 216-triangle asset: g256 -> 96.3%,
// g128 -> 78.7%, g64 -> 30.1%, g32 -> 7.7%. A fixed grid is therefore unusable
// as a level definition across mixed content.
//
// Binary-searches the resolution. Bounded iterations, and it returns the closest
// result it found rather than failing: a level slightly off target is fine, a
// cook that fails because a search did not converge is not.
DecimateResult decimateToRatio(const DecimateInput& in, float targetRatio,
                               int maxIterations = 12);

// Triangle count of a level chain: what a caller should check before shipping a
// level. A level that is not meaningfully cheaper than its parent is worse than
// no level — it costs memory and a swap for nothing, which is precisely the
// state R20 measured.
constexpr float kMinReductionRatio = 0.80f;   // level must be <= 80% of parent

} // namespace meshcook
