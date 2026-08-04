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
inline void writeCullEntry(const RenderItem& it, float& x, float& y, float& z,
                           float& r, uint64_t& keyBase) {
    if (!it.mesh) {                       // not renderable
        x = y = z = 0.0f;
        r = -1.0f;
        keyBase = 0;
        return;
    }
    keyBase = opaqueKeyBase(it.matKey, it.meshKey);
    if (!it.hasBounds) {
        // Unbounded: keep the transform origin as the centre so depth sorting
        // still means something, and make the radius infinite so the plane test
        // can never reject it.
        x = it.model.m[12]; y = it.model.m[13]; z = it.model.m[14];
        r = std::numeric_limits<float>::infinity();
        return;
    }
    const BoundingSphere s = worldSphere(it.model.m, it.boundsCenter, it.boundsSize);
    x = s.x; y = s.y; z = s.z; r = s.radius;
}

// Owning storage for the streams. Kept across frames for its capacity, like the
// rest of the render-side scratch.
struct CullStreamStore {
    std::vector<float>    x, y, z, r;
    std::vector<uint64_t> keyBase;

    void resize(std::size_t n) {
        x.resize(n); y.resize(n); z.resize(n); r.resize(n); keyBase.resize(n);
    }
    std::size_t size() const { return r.size(); }

    CullStreams view() const {
        CullStreams s;
        s.x = { x.data(), x.size() };
        s.y = { y.data(), y.size() };
        s.z = { z.data(), z.size() };
        s.r = { r.data(), r.size() };
        s.keyBase = { keyBase.data(), keyBase.size() };
        return s;
    }
};

// Fill `out` from `items`. For callers that are not fusing this into a pass they
// were already making over the items.
inline void fillCullStream(const Span<RenderItem>& items, CullStreamStore& out) {
    out.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
        writeCullEntry(items[i], out.x[i], out.y[i], out.z[i], out.r[i],
                       out.keyBase[i]);
}

} // namespace rworld
