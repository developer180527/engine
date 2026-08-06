#pragma once
// ── LOD selection — one pure function, no GPU, no ECS ────────────────────────
//
// ONE concern: given how big a thing appears on screen, which detail level draws?
//
// GPU-free like the rest of `world/`, and deliberately a free function over scalars
// rather than a method on a component: level selection is the part of an LOD system
// that fails SILENTLY. A wrong threshold does not crash, it makes props pop between
// detail levels as the camera moves — the hardest class of rendering bug to notice
// in a screenshot and one of the easiest to assert in a test.
//
// THE METRIC IS SCREEN HEIGHT, NOT DISTANCE. Distance thresholds are wrong under two
// conditions this engine already supports: a changed field of view (zooming a scope
// must not coarsen the world) and mixed object scale (a cathedral and a pebble at
// 50 m do not deserve the same detail). What matters is how much of the screen the
// object covers — what Unreal calls "screen size" and Unity "screen relative
// transition height".
//
// For a perspective projection, an object of world radius r at view depth d covers
// this fraction of the viewport HEIGHT:
//
//     h = r * projYScale / d        where projYScale = 1 / tan(fovY / 2)
//
// and `projYScale` is already sitting in the projection matrix at m[5], so no camera
// state has to be plumbed here or kept in sync. Being a ratio, it is also resolution
// independent: the same thresholds hold at 1080p and 4K, which a pixel-count metric
// would not.
//
// LEVEL 0 IS NOT DESCRIBED HERE. The finest mesh is `MeshRenderer::mesh` — the LOD
// chain (`components/lod_mesh.h`) holds only the COARSER levels. That is Unreal's
// shape rather than Unity's, and it buys two things: one source of truth for "what
// mesh is this entity", and removing the chain degrades to full detail instead of
// to nothing. It also means level 0 needs no threshold, so there is no phantom
// entry in the array to get wrong.
#include <cstdint>

namespace rworld {

// Four levels TOTAL — level 0 plus three coarser. Not a guess: three is what most
// kits ship (near, far, billboard) and the fourth leaves room for an impostor
// without making the component variable-length, which would put an allocation in
// the extraction path.
constexpr uint8_t kMaxLodLevels = 4;

// Fraction of the viewport height this sphere covers.
//
// Depth is clamped away from zero. An object straddling the camera would otherwise
// divide by ~0 and produce inf, which does happen to select the finest level — but
// by accident, and after generating an inf that a caller might store or compare
// further. The clamp makes "at the near plane means full detail" deliberate.
inline float lodScreenHeight(float radius, float viewDepth, float projYScale) {
    const float d = viewDepth < 1e-4f ? 1e-4f : viewDepth;
    return radius * projYScale / d;
}

// Which level draws.
//
//   extraLevels    how many COARSER levels exist (0 => always level 0)
//   coarsenBelow   coarsenBelow[i] is the screen height below which level i+1
//                  replaces level i. Descending, e.g. { 0.30, 0.10, 0.03 }.
//
// Contract, and the reason this is worth testing rather than inlining:
//   * always returns a valid level, 0..extraLevels — a chain can never make an
//     object vanish, which is what a "cull below the last threshold" rule would
//     do and is a separate feature (max draw distance) on purpose;
//   * MONOTONE in h: a larger screen height never selects a coarser level. That
//     is the property that stops popping, and it is what the test pins;
//   * a MIS-ORDERED chain is conservative, never invalid. The scan stops at the
//     first threshold h clears, so thresholds that fail to descend can only
//     select a FINER level than intended — an authoring mistake shows up as
//     wasted triangles, not as a hole in the frame.
inline uint8_t selectLod(uint8_t extraLevels, const float* coarsenBelow, float h) {
    if (extraLevels == 0 || !coarsenBelow) return 0;
    if (extraLevels > kMaxLodLevels - 1) extraLevels = kMaxLodLevels - 1;
    uint8_t level = 0;
    while (level < extraLevels && h < coarsenBelow[level]) ++level;
    return level;
}

} // namespace rworld
