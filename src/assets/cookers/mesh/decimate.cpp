#include "assets/cookers/mesh/decimate.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

namespace meshcook {

namespace {

struct Cell {
    int32_t x, y, z;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    size_t operator()(const Cell& c) const {
        // Three odd primes: cheap, and good enough for a per-cook map that is
        // never adversarial. Not a security hash.
        return (size_t)(c.x * 73856093) ^ (size_t)(c.y * 19349663)
             ^ (size_t)(c.z * 83492791);
    }
};

} // namespace

DecimateResult decimate(const DecimateInput& in, uint32_t gridResolution) {
    DecimateResult out;

    if (in.stride < in.posOffset + 12)    { out.error = "stride cannot hold a position"; return out; }
    if (in.indexCount % 3 != 0)           { out.error = "index count is not a multiple of 3"; return out; }
    // Emptiness is checked BEFORE nullness: an empty std::vector's data() is
    // nullptr, so testing null first rejected a legitimately empty mesh as
    // corrupt. Null with a nonzero count is still an error.
    if (in.vertexCount == 0 || in.indexCount == 0) { out.ok = true;              return out; }
    if (!in.vertices || !in.indices)      { out.error = "null input";            return out; }
    if (gridResolution < 1)               { out.error = "grid resolution must be >= 1"; return out; }

    auto positionOf = [&](uint32_t v) -> const float* {
        return (const float*)(in.vertices + (size_t)v * in.stride + in.posOffset);
    };

    // ── bounds ──────────────────────────────────────────────────────────────
    float mn[3] = {  std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max() };
    float mx[3] = { -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max() };
    for (uint32_t v = 0; v < in.vertexCount; ++v) {
        const float* p = positionOf(v);
        for (int i = 0; i < 3; ++i) {
            // NaN fails both comparisons and is simply skipped rather than
            // poisoning the bounds — a single bad vertex must not collapse the
            // whole mesh into one cell.
            if (p[i] < mn[i]) mn[i] = p[i];
            if (p[i] > mx[i]) mx[i] = p[i];
        }
    }
    const float ext[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
    float longest = ext[0];
    if (ext[1] > longest) longest = ext[1];
    if (ext[2] > longest) longest = ext[2];

    // A degenerate mesh (all vertices coincident, or bounds never initialised
    // because every position was NaN) has no grid to speak of. Return it
    // unchanged rather than dividing by zero.
    if (!(longest > 0.0f)) {
        out.vertices.assign(in.vertices, in.vertices + (size_t)in.vertexCount * in.stride);
        out.indices.assign(in.indices, in.indices + in.indexCount);
        out.triangles = in.indexCount / 3;
        out.ok = true;
        return out;
    }
    const float cellSize = longest / (float)gridResolution;

    // ── vertex -> cell -> representative ────────────────────────────────────
    std::unordered_map<Cell, uint32_t, CellHash> repOfCell;
    repOfCell.reserve(in.vertexCount);
    std::vector<uint32_t> remap(in.vertexCount, UINT32_MAX);

    for (uint32_t v = 0; v < in.vertexCount; ++v) {
        const float* p = positionOf(v);
        Cell c{ (int32_t)std::floor((p[0] - mn[0]) / cellSize),
                (int32_t)std::floor((p[1] - mn[1]) / cellSize),
                (int32_t)std::floor((p[2] - mn[2]) / cellSize) };
        // First vertex in index order wins — see the header for why not a
        // centroid. Also makes the result independent of map iteration order.
        const auto [it, inserted] = repOfCell.emplace(c, v);
        remap[v] = it->second;
    }

    // ── rebuild triangles, dropping the collapsed ones ──────────────────────
    // Surviving vertices are compacted so the level carries only what it uses:
    // a level that keeps the parent's whole vertex buffer is cheaper to draw
    // but not cheaper to store, and VRAM is the tighter budget.
    std::unordered_map<uint32_t, uint32_t> compact;   // original rep -> new index
    compact.reserve(repOfCell.size());
    out.vertices.reserve((size_t)repOfCell.size() * in.stride);
    out.indices.reserve(in.indexCount);

    auto emit = [&](uint32_t rep) -> uint32_t {
        const auto it = compact.find(rep);
        if (it != compact.end()) return it->second;
        const uint32_t ni = (uint32_t)(out.vertices.size() / in.stride);
        const uint8_t* src = in.vertices + (size_t)rep * in.stride;
        out.vertices.insert(out.vertices.end(), src, src + in.stride);
        compact.emplace(rep, ni);
        return ni;
    };

    for (uint32_t i = 0; i + 2 < in.indexCount; i += 3) {
        const uint32_t a = in.indices[i], b = in.indices[i + 1], c = in.indices[i + 2];
        // An out-of-range index is corrupt input, not a triangle. Cooked meshes
        // come through a shared DDC, so this is reachable.
        if (a >= in.vertexCount || b >= in.vertexCount || c >= in.vertexCount) continue;
        const uint32_t ra = remap[a], rb = remap[b], rc = remap[c];
        // Two corners in one cell means zero area. This is where the reduction
        // actually comes from.
        if (ra == rb || rb == rc || ra == rc) continue;
        out.indices.push_back(emit(ra));
        out.indices.push_back(emit(rb));
        out.indices.push_back(emit(rc));
    }

    out.triangles = (uint32_t)(out.indices.size() / 3);
    out.ok = true;
    return out;
}

DecimateResult decimateToRatio(const DecimateInput& in, float targetRatio,
                               int maxIterations) {
    if (!(targetRatio > 0.0f) || targetRatio >= 1.0f) {
        DecimateResult bad;
        bad.error = "target ratio must be in (0,1)";
        return bad;
    }
    const uint32_t baseTris = in.indexCount / 3;
    if (baseTris == 0) return decimate(in, 1);

    const uint32_t want = (uint32_t)(baseTris * targetRatio);

    // Resolution is monotone in triangle count (asserted in the test), so a
    // bisection is valid. Bounds: 1 cell collapses everything, and a resolution
    // past the vertex count cannot merge anything further.
    uint32_t lo = 1, hi = 1024;
    DecimateResult best = decimate(in, lo);
    uint32_t bestErr = best.triangles > want ? best.triangles - want
                                             : want - best.triangles;

    for (int i = 0; i < maxIterations && lo < hi; ++i) {
        const uint32_t mid = lo + (hi - lo) / 2;
        DecimateResult r = decimate(in, mid);
        if (!r.ok) break;
        const uint32_t tris = r.triangles;          // read BEFORE any move
        const uint32_t err  = tris > want ? tris - want : want - tris;
        if (err < bestErr) { bestErr = err; best = std::move(r); }
        if (tris > want) { if (mid == 0) break; hi = mid - 1; } else lo = mid + 1;
    }
    return best;
}

} // namespace meshcook
