#pragma once
// ── Filling the cull streams — one definition, two callers ───────────────────
//
// ONE concern: turn a RenderItem into the 24 bytes the cull reads (see
// CullStreams in render_world.h).
//
// It is a header because the important caller is renderer/extract.cpp, which fills
// the streams inside its parallel per-archetype loop while the model matrix is
// already hot — the whole point is not to walk the items a second time. The bulk
// helper below exists for callers that already have an item array and no reason to
// fuse: tests, tools, and anything hand-building a RenderWorld.
//
// Both go through `writeCullEntry`, deliberately. A fused fast path plus a separate
// bulk path is two implementations of one rule, and the bulk one is what the tests
// exercise while the fused one is what ships — precisely the drift that makes a
// green suite meaningless.
#include "render/world/frustum.h"
#include "render/world/render_world.h"
#include "render/world/sort_key.h"

#include <limits>
#include <vector>

namespace rworld {

// The single rule. See CullStreams for what the radius sentinels mean.
inline void writeCullEntry(const RenderItem& it, CullSphere& out,
                           uint64_t& keyBase) {
    if (!it.mesh) {                       // not renderable
        out = CullSphere{};
        out.r = -1.0f;
        keyBase = 0;
        return;
    }
    keyBase = opaqueKeyBase(it.matKey, it.meshKey);
    if (!it.hasBounds) {
        // Unbounded: keep the transform origin as the centre so depth sorting
        // still means something, and make the radius infinite so the plane test
        // can never reject it.
        out.x = it.model.m[12]; out.y = it.model.m[13]; out.z = it.model.m[14];
        out.r = std::numeric_limits<float>::infinity();
        return;
    }
    const BoundingSphere s = worldSphere(it.model.m, it.boundsCenter, it.boundsSize);
    out.x = s.x; out.y = s.y; out.z = s.z; out.r = s.radius;
}

// Owning storage for the streams. Kept across frames for its capacity, like the
// rest of the render-side scratch.
struct CullStreamStore {
    std::vector<CullSphere> sphere;
    std::vector<uint64_t>   keyBase;

    void resize(std::size_t n) { sphere.resize(n); keyBase.resize(n); }
    std::size_t size() const { return sphere.size(); }

    CullStreams view() const {
        CullStreams s;
        s.sphere  = { sphere.data(),  sphere.size() };
        s.keyBase = { keyBase.data(), keyBase.size() };
        return s;
    }
};

// Fill `out` from `items`. For callers that are not fusing this into a pass they
// were already making over the items.
inline void fillCullStream(const Span<RenderItem>& items, CullStreamStore& out) {
    out.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
        writeCullEntry(items[i], out.sphere[i], out.keyBase[i]);
}

} // namespace rworld
