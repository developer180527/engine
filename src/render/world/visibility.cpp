#include "render/world/visibility.h"
#include "render/world/frustum.h"
#include "render/world/draw_sort.h"

#include <algorithm>
#include <cmath>
#include <cstring>            // memmove, for the compaction pass

#include "core/profiler.h"

namespace rworld {
namespace {
// Items per parallel range. The per-item test is cheap, so a range must be big
// enough to be worth more than its own dispatch.
constexpr uint32_t kCullGrain = 2048;
// Below this, threading loses to its own overhead.
constexpr uint32_t kCullParallelMin = 4096;
} // namespace

void buildVisibleSet(const RenderWorld& world, const ViewCamera& cam,
                     VisibleSet& out, const ParallelForFn* parallel) {
    out.draws.clear();                 // keeps capacity — see the header
    const uint32_t n    = (uint32_t)world.items.size();
    out.consideredCount = n;
    out.culledCount     = 0;
    if (n == 0) return;

    // Distance is collected first and normalized after, because the sort key
    // wants depth in [0,1] and the frame's actual depth range is not known
    // until every item has been examined. Normalizing against a guessed far
    // plane instead would waste most of the 24-bit depth precision on empty
    // space in a small scene.
    //
    // A survivor is written at ITS OWN ITEM INDEX rather than appended, so two
    // ranges never touch the same memory and the surviving order is item order
    // however the work was scheduled. The gaps left by culled items are closed
    // afterwards. That is what makes the parallel and serial results identical
    // rather than merely equivalent.
    out.survivors.resize(n);

    const uint32_t rangeCount = (n + kCullGrain - 1) / kCullGrain;
    out.ranges.assign(rangeCount, VisibleSet::Range{});

    // The per-range body. ONE implementation, whether it runs on the job pool or
    // in a loop right here — a separate serial version would be the one every
    // test exercises and the parallel one the version that ships.
    auto cullRange = [&](uint32_t r) {
        const uint32_t begin = r * kCullGrain;
        const uint32_t end   = begin + kCullGrain < n ? begin + kCullGrain : n;
        VisibleSet::Survivor* dst = out.survivors.data() + begin;
        uint32_t written = 0, culled = 0;
        float maxDist = 0.0f;

        for (uint32_t i = begin; i < end; ++i) {
            const RenderItem& it = world.items[i];
            if (!it.mesh) continue;        // not renderable

            // Items without bounds are never culled: an unbounded mesh is a
            // missing-data problem, and silently dropping it would look like a
            // rendering bug rather than the asset issue it is.
            if (it.hasBounds) {
                const BoundingSphere s =
                    worldSphere(it.model.m, it.boundsCenter, it.boundsSize);
                if (outsideFrustum(s, cam.frustum)) { ++culled; continue; }
            }

            const float dx = it.model.m[12] - cam.camPos.x;
            const float dy = it.model.m[13] - cam.camPos.y;
            const float dz = it.model.m[14] - cam.camPos.z;
            const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d > maxDist) maxDist = d;

            dst[written++] = { i, d, it.matKey, it.meshKey };
        }
        // Written once, at the end: the accumulators live in the range's own
        // slot, so no two threads share a counter.
        VisibleSet::Range& acc = out.ranges[r];
        acc.written = written;
        acc.culled  = culled;
        acc.maxDist = maxDist;
    };

    { ENGINE_PROFILE_SCOPE("Cull.test");
      const bool threaded = parallel && *parallel
                         && n >= kCullParallelMin && rangeCount > 1;
      if (threaded) {
          (*parallel)(rangeCount, 1, [&](uint32_t b, uint32_t e) {
              for (uint32_t r = b; r < e; ++r) cullRange(r);
          });
      } else {
          for (uint32_t r = 0; r < rangeCount; ++r) cullRange(r);
      }
    }

    // Reduce the per-range accumulators and close the gaps between slices. One
    // memmove per range, and only for ranges that did not fill their slice —
    // i.e. whenever anything was culled, which is the normal case.
    float maxDist = 0.0f;
    uint32_t total = 0;
    { ENGINE_PROFILE_SCOPE("Cull.compact");
      for (uint32_t r = 0; r < rangeCount; ++r) {
          const VisibleSet::Range& acc = out.ranges[r];
          out.culledCount += acc.culled;
          if (acc.maxDist > maxDist) maxDist = acc.maxDist;
          const uint32_t src = r * kCullGrain;
          if (total != src && acc.written)
              std::memmove(out.survivors.data() + total,
                           out.survivors.data() + src,
                           acc.written * sizeof(VisibleSet::Survivor));
          total += acc.written;
      }
    }

    { ENGINE_PROFILE_SCOPE("Cull.key");
      const float inv = maxDist > 0.0f ? 1.0f / maxDist : 0.0f;
      out.draws.resize(total);
      for (uint32_t i = 0; i < total; ++i) {
          const VisibleSet::Survivor& p = out.survivors[i];
          // Blend class is not yet carried by RenderItem — every draw is opaque
          // today (the pipeline has no transparent path wired). Stated here
          // rather than hidden, so the sort key is honest about what it knows.
          out.draws[i] = { makeSortKey(BlendClass::Opaque, p.dist * inv,
                                       p.mat, p.mesh), p.index };
      }
    }

    { ENGINE_PROFILE_SCOPE("Cull.sort");
      // Radix, not std::sort: the keys are unsigned integers, and this was 70% of
      // the cull. Also STABLE, which std::sort was not — see draw_sort.h.
      sortDraws(out.draws, out.sortScratch);
    }
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
