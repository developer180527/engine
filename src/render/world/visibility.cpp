#include "render/world/visibility.h"
#include "render/world/frustum.h"
#include "render/world/draw_sort.h"

#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstring>            // memmove, for the compaction pass
#include <limits>
#include <thread>             // hardware_concurrency — a grain hint, see cullGrain

#include "core/profiler.h"

namespace rworld {
namespace {
// Below this, threading loses to its own overhead.
constexpr uint32_t kCullParallelMin = 4096;
// A range must be worth more than its own dispatch, so never go below this.
constexpr uint32_t kCullGrainFloor = 512;

// Items per parallel range, SIZED TO THE MACHINE (issues.md A2.P3). A fixed 2048
// made the grain wrong at both ends: 5 000 items became three ranges, so a
// 10-core machine left seven cores idle, while a huge scene produced far more
// ranges than it needed. Aim for ~4 ranges per hardware thread — enough for the
// scheduler to balance uneven ranges, few enough that dispatch stays noise.
//
// hardware_concurrency is a HINT, not the pool size: rworld deliberately does not
// know about the job facade (see ParallelForFn), so it cannot ask how many workers
// exist. Over- or under-estimating only changes the range count, never the result.
uint32_t cullGrain(uint32_t n) {
    static const uint32_t kThreads = [] {
        const unsigned hc = std::thread::hardware_concurrency();
        return hc ? (uint32_t)hc : 4u;
    }();
    const uint32_t target = n / (kThreads * 4);
    return target < kCullGrainFloor ? kCullGrainFloor : target;
}
} // namespace

void buildVisibleSet(const RenderWorld& world, const ViewCamera& cam,
                     VisibleSet& out, const ParallelForFn* parallel) {
    out.draws.clear();                 // keeps capacity — see the header
    const uint32_t n    = (uint32_t)world.items.size();
    out.consideredCount = n;
    out.culledCount     = 0;
    if (n == 0) return;

    // The streams ARE the input; there is no fallback that rebuilds them from the
    // items. A second path would be the one the tests exercise while the fused one
    // ships. Callers that are not extraction use rworld::fillCullStream.
    assert(world.cull.consistent(n)
           && "RenderWorld::cull must be filled and parallel to items — see "
              "rworld::fillCullStream");
    if (!world.cull.consistent(n)) return;

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

    const uint32_t grain = cullGrain(n);
    const uint32_t rangeCount = (n + grain - 1) / grain;
    out.ranges.assign(rangeCount, VisibleSet::Range{});

    // The per-range body. ONE implementation, whether it runs on the job pool or
    // in a loop right here — a separate serial version would be the one every
    // test exercises and the parallel one the version that ships.
    //
    // Reads the streams, never the items: 24 bytes per item instead of the 144-byte
    // RenderItem whose three cache lines this loop used to walk, and the bounding
    // sphere arrives precomputed rather than being rebuilt from a matrix once per
    // view. Two sequential streams, not five — see CullStreams for the measurement
    // that chose an interleaved sphere over four parallel arrays.
    const CullSphere* SP = world.cull.sphere.data;
    const uint64_t*   SK = world.cull.keyBase.data;

    auto cullRange = [&](uint32_t r) {
        const uint32_t begin = r * grain;
        const uint32_t end   = begin + grain < n ? begin + grain : n;
        VisibleSet::Survivor* dst = out.survivors.data() + begin;
        uint32_t written = 0, culled = 0;
        float maxDist = 0.0f;
        const float* V = cam.view.m;

        for (uint32_t i = begin; i < end; ++i) {
            const CullSphere s = SP[i];       // one 16-byte sequential read
            if (s.r < 0.0f) continue;         // not renderable — see the sentinels

            const float cx = s.x, cy = s.y, cz = s.z;
            // An infinite radius can never be rejected, so the test is skipped
            // rather than relying on -inf comparisons behaving.
            if (s.r != std::numeric_limits<float>::infinity()) {
                BoundingSphere sp; sp.x = cx; sp.y = cy; sp.z = cz; sp.radius = s.r;
                if (outsideFrustum(sp, cam.frustum)) { ++culled; continue; }
            }

            // VIEW-SPACE depth of the sphere centre. Row-vector convention: view
            // space z is the third column of `view`. Magnitude, so a caller's
            // handedness cannot collapse every depth to zero.
            const float zv = cx*V[2] + cy*V[6] + cz*V[10] + V[14];
            const float d  = zv < 0.0f ? -zv : zv;
            if (d > maxDist) maxDist = d;

            dst[written++] = { SK[i], i, d };
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
          const uint32_t src = r * grain;
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
          // Only the depth field is added here; material and mesh were packed at
          // extraction. Blend class is not yet carried by RenderItem — every draw
          // is opaque today (the pipeline has no transparent path wired). Stated
          // rather than hidden, so the sort key is honest about what it knows.
          out.draws[i] = { withOpaqueDepth(p.keyBase, p.dist * inv), p.index };
      }
    }

    { ENGINE_PROFILE_SCOPE("Cull.sort");
      // Radix, not std::sort: the keys are unsigned integers, and this was 70% of
      // the cull. Also STABLE, which std::sort was not — see draw_sort.h.
      sortDraws(out.draws, out.sortScratch);
    }
}

std::size_t batchRunLength(const std::vector<VisibleDraw>& draws, std::size_t first) {
    if (first >= draws.size()) return 0;
    std::size_t n = 1;
    while (first + n < draws.size()
           && sameBatch(draws[first].key, draws[first + n].key)
           && draws[first].submesh == draws[first + n].submesh)
        ++n;
    return n;
}

} // namespace rworld
