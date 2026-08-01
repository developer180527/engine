#include "render/world/visibility.h"
#include "render/world/frustum.h"

#include <algorithm>
#include <cmath>

namespace rworld {

void buildVisibleSet(const RenderWorld& world, const ViewCamera& cam,
                     VisibleSet& out) {
    out.draws.clear();                 // keeps capacity — see the header
    out.consideredCount = (uint32_t)world.items.size();
    out.culledCount     = 0;

    // Distance is collected first and normalized after, because the sort key
    // wants depth in [0,1] and the frame's actual depth range is not known
    // until every item has been examined. Normalizing against a guessed far
    // plane instead would waste most of the 24-bit depth precision on empty
    // space in a small scene.
    struct Pending { uint32_t index; float dist; uint32_t mat, mesh; };
    static thread_local std::vector<Pending> pending;
    pending.clear();
    pending.reserve(world.items.size());

    float maxDist = 0.0f;
    for (uint32_t i = 0; i < (uint32_t)world.items.size(); ++i) {
        const RenderItem& it = world.items[i];
        if (!it.mesh) continue;        // not renderable

        // Items without bounds are never culled: an unbounded mesh is a
        // missing-data problem, and silently dropping it would look like a
        // rendering bug rather than the asset issue it is.
        if (it.hasBounds) {
            const BoundingSphere s =
                worldSphere(it.model.m, it.boundsCenter, it.boundsSize);
            if (outsideFrustum(s, cam.frustum)) { ++out.culledCount; continue; }
        }

        const float dx = it.model.m[12] - cam.camPos.x;
        const float dy = it.model.m[13] - cam.camPos.y;
        const float dz = it.model.m[14] - cam.camPos.z;
        const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (d > maxDist) maxDist = d;

        pending.push_back({ i, d, it.matKey, it.meshKey });
    }

    const float inv = maxDist > 0.0f ? 1.0f / maxDist : 0.0f;
    out.draws.reserve(pending.size());
    for (const auto& p : pending) {
        // Blend class is not yet carried by RenderItem — every draw is opaque
        // today (the pipeline has no transparent path wired). Stated here
        // rather than hidden, so the sort key is honest about what it knows.
        out.draws.push_back({ makeSortKey(BlendClass::Opaque, p.dist * inv,
                                          p.mat, p.mesh), p.index });
    }

    std::sort(out.draws.begin(), out.draws.end(),
              [](const VisibleDraw& a, const VisibleDraw& b) {
                  return a.key < b.key;
              });
}

std::size_t batchRunLength(const VisibleSet& set, std::size_t first) {
    if (first >= set.draws.size()) return 0;
    std::size_t n = 1;
    while (first + n < set.draws.size()
           && sameBatch(set.draws[first].key, set.draws[first + n].key))
        ++n;
    return n;
}

} // namespace rworld
