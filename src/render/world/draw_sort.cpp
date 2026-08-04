#include "render/world/draw_sort.h"

#include <cstdint>

namespace rworld {
namespace {
constexpr int      kBytes   = 8;      // a 64-bit key
constexpr uint32_t kBuckets = 256;    // one byte at a time: histograms stay in L1

// A 16-bit digit would halve the pass count, but its histogram is 65 536
// counters — clearing 256 KB per pass costs more than sorting 19 000 draws.
// 256 counters per byte, all eight histograms built in ONE read of the keys.
} // namespace

void sortDraws(std::vector<VisibleDraw>& draws, std::vector<VisibleDraw>& scratch) {
    const std::size_t n = draws.size();
    if (n < 2) return;

    // Every histogram from a single pass over the keys. Reading the keys eight
    // times to count would cost more than the sort itself.
    uint32_t hist[kBytes][kBuckets] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t k = draws[i].key;
        for (int b = 0; b < kBytes; ++b)
            ++hist[b][(k >> (b * 8)) & 0xFFu];
    }

    scratch.resize(n);
    std::vector<VisibleDraw>* src = &draws;
    std::vector<VisibleDraw>* dst = &scratch;
    int passes = 0;

    for (int b = 0; b < kBytes; ++b) {
        // A byte where one bucket holds everything carries no information — every
        // key agrees on it, so a pass would copy the array for nothing. This is
        // the common case for the high bytes (blend class, material, mesh), not a
        // special case: it is why the sort costs three passes on a scene drawing
        // one material rather than eight.
        bool varies = true;
        for (uint32_t v = 0; v < kBuckets; ++v)
            if (hist[b][v] == n) { varies = false; break; }
        if (!varies) continue;

        // Prefix sums turn counts into output offsets. Walking the source in
        // order and taking each bucket's next slot is what makes this STABLE.
        uint32_t offset[kBuckets];
        uint32_t sum = 0;
        for (uint32_t v = 0; v < kBuckets; ++v) {
            offset[v] = sum;
            sum += hist[b][v];
        }

        const VisibleDraw* in = src->data();
        VisibleDraw*       ou = dst->data();
        const int shift = b * 8;
        for (std::size_t i = 0; i < n; ++i)
            ou[offset[(in[i].key >> shift) & 0xFFu]++] = in[i];

        std::swap(src, dst);
        ++passes;
    }

    // The result must end up in `draws` however many passes ran. An odd count
    // leaves it in scratch — the alternative (always doing an even number) would
    // mean a pointless full copy, and the alternative to THAT (returning which
    // buffer holds the answer) would push the bookkeeping onto every caller.
    if (src != &draws) draws.swap(scratch);
    (void)passes;
}

} // namespace rworld
