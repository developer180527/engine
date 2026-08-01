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
// point nobody can actually use (docs/renderer-architecture.md §3).
//
// GPU-free by construction — RenderItem carries its own bounds, so nothing
// here touches Mesh or bgfx and all of it is unit-testable.
#include "render/world/render_world.h"
#include "render/world/sort_key.h"

#include <cstdint>
#include <vector>

namespace rworld {

// One draw, ordered. `index` refers back into RenderWorld::items — the item
// itself is not copied, since the sort only needs the key.
struct VisibleDraw {
    uint64_t key   = 0;
    uint32_t index = 0;
};

struct VisibleSet {
    std::vector<VisibleDraw> draws;    // sorted ascending by key
    uint32_t consideredCount = 0;      // items examined
    uint32_t culledCount     = 0;      // rejected by the frustum

    bool empty() const { return draws.empty(); }
    std::size_t size() const { return draws.size(); }
};

// Cull against the camera frustum, assign sort keys, sort.
//
// `out` is reused across frames on purpose: this runs once per view per frame
// (and shadow cascades multiply that), so the vector's capacity should persist
// rather than being reallocated every frame.
void buildVisibleSet(const RenderWorld& world, const ViewCamera& cam,
                     VisibleSet& out);

// Number of draws at the start of the batch run beginning at `first` — i.e.
// how many consecutive draws share mesh AND material. Returns >= 1.
// The instancing hook: a run longer than 1 can collapse into one submit.
std::size_t batchRunLength(const VisibleSet& set, std::size_t first);

} // namespace rworld
