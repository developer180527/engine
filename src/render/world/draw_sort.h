#pragma once
// ── Sorting the draw list — one concern ──────────────────────────────────────
//
// ONE concern: order a VisibleDraw array by its 64-bit sort key, ascending.
//
// Its own unit because it stopped being a one-liner. `std::sort` with a key
// comparator was 1.15 ms of a 1.6 ms cull at 50 000 objects — 70% of the phase,
// and the largest single cost left in the render path after extraction and the
// frustum test were parallelised. A comparison sort spends n·log n ≈ 270 000
// comparisons on 19 000 draws; the keys are unsigned integers, so they can be
// sorted by counting instead, in a fixed number of linear passes.
//
// TWO PROPERTIES THAT MATTER MORE THAN THE SPEED:
//
//   * STABLE. LSD radix is stable by construction, so draws with identical keys
//     keep item order. `std::sort` is not stable, so the previous behaviour left
//     tie order unspecified — two runs of the same frame could order coincident
//     draws differently, and an instanced run could pack its matrices in a
//     different sequence. Determinism here is worth having on its own: it makes
//     "byte-identical submit counters" a meaningful claim about a frame.
//
//   * DATA-ADAPTIVE. Bytes that are constant across every key are skipped, and
//     that is the common case rather than an optimisation for a benchmark: the
//     key's high bits are blend class (always opaque today) plus material and
//     mesh ids, so a scene drawing a few materials leaves the top five bytes
//     identical and only the depth bytes need passing. A single histogram pass
//     discovers which bytes vary, so the cost tracks the entropy actually
//     present instead of the width of the key.
#include "render/world/visibility.h"

#include <vector>

namespace rworld {

// Sort `draws` ascending by key, stably. `scratch` is a caller-owned buffer,
// reused across frames for its capacity — the sort ping-pongs between the two
// and leaves the result in `draws` whatever the number of passes.
void sortDraws(std::vector<VisibleDraw>& draws, std::vector<VisibleDraw>& scratch);

} // namespace rworld
