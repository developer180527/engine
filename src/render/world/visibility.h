#pragma once
// ── Visibility — cull, key, sort ────────────────────────────────────────────
//
// ONE concern: given a RenderWorld and a camera, which draws happen and in
// what order?
//
// This is the machinery half of the renderer, and the reason it now lives here
// instead of inside ForwardPipeline: a project that swaps the pipeline to
// change how surfaces LOOK should not have to reimplement culling and sorting
// to do it. Today it must, which is why `IRenderPipeline` is a customization
// point nobody can actually use (docs/architecture/renderer-architecture.md §3).
//
// GPU-free by construction — RenderItem carries its own bounds, so nothing
// here touches Mesh or bgfx and all of it is unit-testable.
#include "render/world/render_world.h"
#include "render/world/sort_key.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace rworld {

// ── ParallelForFn — how this layer gets threads without depending on them ───
// The cull is the largest phase in the render path at scene scale, and it is a
// pure per-item test, so it wants to run on the job pool. But `rworld` is
// GPU-FREE AND RUNTIME-FREE by construction — that property is what makes all of
// it unit-testable — and including the runtime's job facade here would spend it.
//
// So the caller injects a dispatcher instead. ForwardPipeline passes one backed
// by jobs::parallelFor; a test can pass nullptr for a serial run, or a
// deliberately hostile implementation (ranges executed out of order, or one
// range at a time) to prove the result does not depend on scheduling. That last
// use is why this is injected rather than #ifdef'd.
//
// Contract: BLOCKS until every range has run, calls fn(begin, end) over
// disjoint ranges covering [0, count), and may run them on any thread in any
// order. nullptr means "run it yourself, serially".
using ParallelForFn =
    std::function<void(uint32_t count, uint32_t grain,
                       const std::function<void(uint32_t begin, uint32_t end)>&)>;

// One draw, ordered. `index` refers back into RenderWorld::items — the item
// itself is not copied, since the sort only needs the key.
//
// `submesh` makes a submesh RANGE a first-class draw rather than something expanded
// inside the submit loop. kWholeMesh means "draw the mesh's whole index buffer".
// Before this, one entry per ITEM meant submesh draws could neither batch nor dedup:
// the sort grouped by the item's material, then submission bound A, B, C for each
// item's own ranges, so 96 of a 176-mesh kit could never instance and material binds
// equalled draws exactly. Expanding BEFORE the sort is what lets a range's own
// material into the key.
//
// Still 16 bytes: the padding after `index` was already there.
struct VisibleDraw {
    static constexpr uint16_t kWholeMesh = 0xFFFFu;
    uint64_t key     = 0;
    uint32_t index   = 0;
    uint16_t submesh = kWholeMesh;
};

struct VisibleSet {
    std::vector<VisibleDraw> draws;    // sorted ascending by key
    uint32_t consideredCount = 0;      // items examined
    uint32_t culledCount     = 0;      // rejected by the frustum

    bool empty() const { return draws.empty(); }
    std::size_t size() const { return draws.size(); }

    // ── Scratch, public only so its capacity survives across frames ─────────
    // Not part of the result. A survivor of the cull, before the frame's depth
    // range is known and keys can be built.
    struct Survivor {
        uint64_t keyBase;   // material+mesh, packed at extraction
        uint32_t index;
        float    dist;      // view-space depth, normalised once the range is known
    };
    // One per parallel range: how many survivors it wrote into its slice, and
    // the reductions it computed locally. Kept out of the loop body so no two
    // threads touch the same accumulator.
    struct Range { uint32_t written = 0, culled = 0; float maxDist = 0.0f; };
    std::vector<Survivor> survivors;
    std::vector<Range>    ranges;
    std::vector<VisibleDraw> sortScratch;   // ping-pong buffer for sortDraws
};

// Cull against the camera frustum, assign sort keys, sort.
//
// `out` is reused across frames on purpose: this runs once per view per frame
// (and shadow cascades multiply that), so the vector's capacity should persist
// rather than being reallocated every frame.
// `parallel` may be null (run serially). See ParallelForFn: the RESULT IS
// IDENTICAL either way — survivors keep item order and the sort is by key — so
// which path ran is a performance question, never a correctness one.
void buildVisibleSet(const RenderWorld& world, const ViewCamera& cam,
                     VisibleSet& out, const ParallelForFn* parallel = nullptr);

// Number of draws at the start of the batch run beginning at `first` — i.e. how many
// consecutive draws share mesh AND material AND the same submesh range. Returns >= 1.
// The instancing hook: a run longer than 1 can collapse into one submit.
//
// The submesh comparison is not optional. Two ranges of one mesh that happen to share
// a material produce EQUAL keys (material+mesh are all the key holds) while needing
// different index ranges — instancing them together would draw one range's indices for
// the other's geometry.
std::size_t batchRunLength(const std::vector<VisibleDraw>& draws, std::size_t first);
// Convenience for the common case of asking about a VisibleSet's own list.
inline std::size_t batchRunLength(const VisibleSet& set, std::size_t first) {
    return batchRunLength(set.draws, first);
}

} // namespace rworld
