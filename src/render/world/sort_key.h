#pragma once
// ── Sort keys — one integer per draw ────────────────────────────────────────
//
// ONE concern: turn "how should this draw be ordered?" into a number.
//
// Replaces a comparator lambda that compared meshKey then matKey field by
// field. Packing into a single uint64 buys three things:
//   • the sort is one integer compare instead of a branchy predicate;
//   • ordering policy becomes DATA, visible and testable, instead of control
//     flow buried in a pipeline;
//   • adjacent equal high bits mean "same mesh + material" — which is exactly
//     the run-detection instancing needs, for free.
//
// BLEND CLASS ALWAYS LEADS: opaque must precede transparent whatever else is
// true, or blending is simply wrong. That is correctness, not tuning.
//
// Below that the layout DIFFERS BY CLASS, because the two have opposite goals:
//
//   Opaque       [blend 2][material 16][mesh 21][depth 24 near-first]
//   Transparent  [blend 2][depth 24 far-first ][material 16][mesh 21]
//
// Transparency MUST be back-to-front — depth dominates or it renders wrong.
//
// Opaque keeps MATERIAL above depth deliberately. The pipeline this replaces
// sorted by (meshKey, matKey) — pure batching — and flipping to depth-first
// would trade that away for early-Z on the strength of a guess. This engine
// currently submits ~13 draws with no measured overdraw, so the honest move is
// to preserve existing behaviour and revisit when the profile says overdraw
// costs more than state changes. Depth still rides along as the tiebreaker, so
// coincident draws in one material go front-to-back for free.
#include <cassert>
#include <cstdint>

namespace rworld {

enum class BlendClass : uint8_t {
    Opaque      = 0,   // front-to-back, early-Z friendly
    AlphaTest   = 1,
    Transparent = 2,   // back-to-front, correctness-critical
};

// `depth01` is normalized [0,1] view depth (0 = nearest). Quantized to 24
// bits, so draws only tie when genuinely coincident.
// FIELD WIDTHS ARE A HARD LIMIT, and overflowing one is silent (issues.md A1.6).
// Material ids get 16 bits and mesh ids 21; both are registry SLOT INDICES, so a
// project with more than 65 535 live materials wraps. What makes that worse than a
// wrong sort is `sameBatch`: it compares these very bits, so two DIFFERENT
// materials that alias in 16 bits are judged the same batch and get collapsed into
// one instanced submit with one material bound — the wrong one. Nothing would
// report it; it renders confidently wrong.
//
// Asserted in debug rather than masked. A release build still truncates, which is
// recorded as the open half of A1.6: catching it properly belongs at registry-add
// time, where a limit can be refused loudly instead of discovered here.
constexpr uint32_t kMaxMaterialKey = 0xFFFFu;      // 16 bits
constexpr uint32_t kMaxMeshKey     = 0x1FFFFFu;    // 21 bits

inline uint64_t makeSortKey(BlendClass blend, float depth01,
                            uint32_t materialKey, uint32_t meshKey) {
    assert(materialKey <= kMaxMaterialKey
           && "material id exceeds the sort key's 16 bits — ids would alias and "
              "sameBatch() would instance two different materials as one");
    assert(meshKey <= kMaxMeshKey
           && "mesh id exceeds the sort key's 21 bits — ids would alias and "
              "sameBatch() would instance two different meshes as one");
    if (depth01 < 0.0f) depth01 = 0.0f;
    if (depth01 > 1.0f) depth01 = 1.0f;

    constexpr uint32_t kDepthMax = 0xFFFFFFu;
    const uint32_t mat  = materialKey & 0xFFFFu;
    const uint32_t mesh = meshKey     & 0x1FFFFFu;
    const uint64_t cls  = (uint64_t)(uint8_t)blend << 61;

    if (blend == BlendClass::Transparent) {
        // FAR first: invert so a single ascending sort serves both classes.
        const uint32_t d = kDepthMax - (uint32_t)(depth01 * (float)kDepthMax);
        return cls | ((uint64_t)d << 37) | ((uint64_t)mat << 21) | mesh;
    }
    // Opaque / alpha-test: material, then mesh, then near-first depth.
    const uint32_t d = (uint32_t)(depth01 * (float)kDepthMax);
    return cls | ((uint64_t)mat << 45) | ((uint64_t)mesh << 24) | d;
}

// ── Split form: the half that does not depend on the camera ─────────────────
// Extraction knows material and mesh; only the cull knows depth. Precomputing the
// base means the cull ORs in 24 bits instead of repacking the whole key per view,
// and it is what lets the cull read a stream instead of RenderItem (see
// CullStreams). `makeSortKey` stays the single-shot form for callers with
// everything in hand, and the two agree by construction:
//     withOpaqueDepth(opaqueKeyBase(m, s), d) == makeSortKey(Opaque, d, m, s)
// which render_world_test asserts over randomised inputs.
//
// TRANSPARENT keys cannot be split this way: their layout puts depth ABOVE
// material, so the base bits move. Not a limitation today — no RenderItem carries
// a blend class yet — but a transparent path must pack its own stream rather than
// reuse this one.
inline uint64_t opaqueKeyBase(uint32_t materialKey, uint32_t meshKey) {
    assert(materialKey <= kMaxMaterialKey);
    assert(meshKey <= kMaxMeshKey);
    const uint64_t cls = (uint64_t)(uint8_t)BlendClass::Opaque << 61;
    return cls | ((uint64_t)(materialKey & 0xFFFFu) << 45)
               | ((uint64_t)(meshKey & 0x1FFFFFu) << 24);
}

// Re-key an existing draw onto a different material without recomputing depth: the
// depth code is already quantised, so it is ORed straight back on. Used when submesh
// ranges are expanded into their own draws and each takes its range's material.
inline uint64_t withOpaqueDepthCode(uint64_t base, uint32_t depthCode) {
    return base | (uint64_t)(depthCode & 0xFFFFFFu);
}

inline uint64_t withOpaqueDepth(uint64_t base, float depth01) {
    if (depth01 < 0.0f) depth01 = 0.0f;
    if (depth01 > 1.0f) depth01 = 1.0f;
    constexpr uint32_t kDepthMax = 0xFFFFFFu;
    return base | (uint64_t)(uint32_t)(depth01 * (float)kDepthMax);
}

// Do two OPAQUE draws share material AND mesh? The instancing predicate: a run
// of these can collapse into one instanced submit. Only meaningful for opaque —
// transparent draws are depth-ordered and cannot be reordered into batches.
inline bool sameBatch(uint64_t a, uint64_t b) {
    constexpr uint64_t kClass = 0x3ull << 61;
    if ((a & kClass) != (b & kClass)) return false;
    if ((BlendClass)(uint8_t)((a >> 61) & 0x3ull) == BlendClass::Transparent)
        return false;
    constexpr uint64_t kMatMesh = (0xFFFFull << 45) | (0x1FFFFFull << 24);
    return (a & kMatMesh) == (b & kMatMesh);
}

inline BlendClass blendOf(uint64_t key) {
    return (BlendClass)(uint8_t)((key >> 61) & 0x3ull);
}
inline uint32_t materialOf(uint64_t key) {
    return blendOf(key) == BlendClass::Transparent
         ? (uint32_t)((key >> 21) & 0xFFFFull)
         : (uint32_t)((key >> 45) & 0xFFFFull);
}
// The quantized depth code. Exposed for the same reason materialOf/meshOf are:
// the ordering policy is data, so a test must be able to read it back. It also
// makes ONE class of bug observable that nothing else does — the frame's depth
// normalisation (maxDist) being under-estimated, which silently CLAMPS every
// draw beyond it to the same saturated code and destroys depth ordering inside a
// material group. A scale error that keeps every draw in range is invisible by
// construction, because ordering is all the key is for.
inline uint32_t depthOf(uint64_t key) {
    return blendOf(key) == BlendClass::Transparent
         ? (uint32_t)((key >> 37) & 0xFFFFFFull)
         : (uint32_t)(key & 0xFFFFFFull);
}

inline uint32_t meshOf(uint64_t key) {
    return blendOf(key) == BlendClass::Transparent
         ? (uint32_t)(key & 0x1FFFFFull)
         : (uint32_t)((key >> 24) & 0x1FFFFFull);
}

} // namespace rworld
