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
        //
        // MULTIPLIED AS UNSIGNED, deliberately. On int32_t this overflows at a
        // cell index of just 30 (30 * 73856093 > INT32_MAX) — which is every
        // real mesh, since the resolution search goes to 1024 — and signed
        // overflow is undefined behaviour, so the UBSan build (-D
        // ENGINE_SANITIZE=undefined) trips on the hash of an ordinary asset.
        // Unsigned wrap is defined and gives the identical bit pattern.
        return (size_t)((uint32_t)c.x * 73856093u) ^ (size_t)((uint32_t)c.y * 19349663u)
             ^ (size_t)((uint32_t)c.z * 83492791u);
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
        // Indices are copied verbatim, so the parent's ranges still describe
        // them exactly — carry them through or this one path would silently
        // return a level that draws with material[0].
        if (in.rangeCount && in.ranges)
            out.ranges.assign(in.ranges, in.ranges + in.rangeCount);
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
        // NON-FINITE POSITIONS GET NO CELL. The bounds pass already skips them
        // (NaN fails both comparisons), but the cell index was still computed
        // from them, and `(int32_t)std::floor(NaN)` is undefined — it yields
        // INT_MIN on x86-64 and 0 on arm64. That would make the cook
        // ARCHITECTURE-DEPENDENT, which breaks the one property the whole
        // algorithm choice rests on: two machines cooking byte-identical levels
        // so the DDC stays a cache rather than a coin flip. Left as UINT32_MAX,
        // and the triangle pass below drops anything referencing it.
        const float fx = (p[0] - mn[0]) / cellSize;
        const float fy = (p[1] - mn[1]) / cellSize;
        const float fz = (p[2] - mn[2]) / cellSize;
        if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(fz))
            continue;
        Cell c{ (int32_t)std::floor(fx),
                (int32_t)std::floor(fy),
                (int32_t)std::floor(fz) };
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

    // ── Rebuilt GROUP BY GROUP, so a level keeps its materials ──────────────
    // The first version rebuilt one flat index buffer and dropped the ranges,
    // which meant every level drew with material[0]: a prop with two material
    // groups CHANGED COLOUR as it crossed an LOD threshold, and 96 of the
    // MegaKit's 176 meshes have more than one group. Clustering is still
    // global (a vertex belongs to one cell whatever group references it), only
    // the index rebuild is partitioned — so this costs nothing and the output
    // ranges come out contiguous from zero, which keeps `submeshesTile()` true
    // and the shadow pass on its one-draw path.
    const SubRange whole{ 0, in.indexCount, 0 };
    const SubRange* ranges    = in.rangeCount ? in.ranges : &whole;
    const uint32_t  rangeCount = in.rangeCount ? in.rangeCount : 1u;
    if (in.rangeCount && !in.ranges) { out.error = "range count without ranges"; return out; }

    for (uint32_t r = 0; r < rangeCount; ++r) {
        const SubRange& src = ranges[r];
        // A range outside the index buffer is corrupt input, like an
        // out-of-range index: skip the group rather than reading past the end.
        if (src.indexCount > in.indexCount ||
            src.indexOffset > in.indexCount - src.indexCount) continue;

        const uint32_t firstOut = (uint32_t)out.indices.size();
        const uint32_t end = src.indexOffset + src.indexCount;
        for (uint32_t i = src.indexOffset; i + 2 < end; i += 3) {
            const uint32_t a = in.indices[i], b = in.indices[i + 1], c = in.indices[i + 2];
            // An out-of-range index is corrupt input, not a triangle. Cooked
            // meshes come through a shared DDC, so this is reachable.
            if (a >= in.vertexCount || b >= in.vertexCount || c >= in.vertexCount) continue;
            const uint32_t ra = remap[a], rb = remap[b], rc = remap[c];
            // A vertex with no cell (non-finite position) has no representative.
            if (ra == UINT32_MAX || rb == UINT32_MAX || rc == UINT32_MAX) continue;
            // Two corners in one cell means zero area. This is where the
            // reduction actually comes from.
            if (ra == rb || rb == rc || ra == rc) continue;
            out.indices.push_back(emit(ra));
            out.indices.push_back(emit(rb));
            out.indices.push_back(emit(rc));
        }

        const uint32_t emitted = (uint32_t)out.indices.size() - firstOut;
        // A group whose every triangle collapsed is dropped, not emitted empty:
        // a zero-index draw still costs a material bind and a submit.
        if (emitted) out.ranges.push_back(SubRange{ firstOut, emitted, src.materialIndex });
    }

    // One group covering everything is not information — it is what an absent
    // table already means, and storing it would make every single-material
    // level carry a range table for nothing.
    if (out.ranges.size() == 1 && out.ranges[0].indexOffset == 0 &&
        out.ranges[0].indexCount == out.indices.size() &&
        out.ranges[0].materialIndex == 0)
        out.ranges.clear();

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
        // `mid >= 1` always, since lo starts at 1 and only grows — the old
        // `if (mid == 0) break` here was unreachable.
        if (tris > want) hi = mid - 1; else lo = mid + 1;
    }
    return best;
}

} // namespace meshcook
