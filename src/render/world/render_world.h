#pragma once
// ── RenderWorld — the frozen description of one frame ───────────────────────
//
// The GPU-FREE half of what used to be render_view.h. Nothing here includes
// bgfx: a frame description is data about *what to draw*, and the graphics API
// is an implementation detail of *how*. Only RenderTarget/RenderView (which
// name framebuffers and view ids) still need bgfx, and they stay behind.
//
// Why the split matters beyond tidiness:
//   • VISIBILITY BECOMES TESTABLE. Culling and sort order are pure functions
//     over these PODs, so they can be asserted with no device — and src/render
//     has no GPU test harness, which is exactly why its pipeline has never had
//     a test that can fail.
//   • PASSES CANNOT CHEAT. A pass handed this cannot reach back into the ECS,
//     so it stays reorderable and reusable.
//   • EXTRACTION CAN BE PARALLEL. Flat arrays of independent items.
//
// See docs/architecture/renderer-architecture.md §5.
#include "core/handle.h"
#include "core/math_types.h"      // Vec3/Vec4/Mat4 (bx math — not the GPU API)
#include "components/light.h"     // LightType

#include <cstddef>
#include <cstdint>

// Resolved at extraction; referenced by pointer only, never dereferenced by
// visibility (see RenderItem::bounds*).
struct Mesh;
struct Material;
struct Texture;

// Minimal non-owning view (avoids requiring C++20 std::span).
template <class T>
struct Span {
    const T* data = nullptr;
    std::size_t n = 0;
    const T* begin() const { return data; }
    const T* end()   const { return data + n; }
    std::size_t size() const { return n; }
    bool empty() const { return n == 0; }
    const T& operator[](std::size_t i) const { return data[i]; }
};

// One light, resolved for the GPU. direction/position are world-space, baked
// from the light entity's Transform at extraction.
struct LightItem {
    LightType type         = LightType::Directional;
    Vec3      direction    { 0.0f, -1.0f, 0.0f };  // toward-light, set at extraction
    Vec3      position     { 0.0f,  0.0f, 0.0f };
    Vec3      color        { 1.0f,  1.0f, 1.0f };
    float     intensity    = 1.0f;
    float     range        = 10.0f;
    float     spotInnerCos = 0.95f;
    float     spotOuterCos = 0.85f;
    bool      castShadows  = false;
};

// One drawable, resolved once at extraction. POD, and FIELD ORDER IS DELIBERATE —
// see the note under the struct before adding anything to it.
struct RenderItem {
    // ── Read by SUBMISSION, hot: first cache line and a bit ─────────────────
    Mat4         model;                    // 64 B: setTransform, or instance data
    const Mesh*  mesh = nullptr;           // vbh/ibh/submeshes/doubleSided
    const float* boneMatrices = nullptr;   // -> SkinnedMesh::skinMatrices, or null

    // ── Read only when BUILDING THE CULL STREAMS, at extraction ─────────────
    // Cold for submission, which never looks at any of it. Kept on the item
    // because rworld::writeCullEntry takes a RenderItem: the item is the INPUT
    // the streams are derived from (see cull_stream.h).
    //
    // LOCAL-SPACE BOUNDS, COPIED AT EXTRACTION. Culling used to reach through
    // `mesh->boundsCenter()` for every item every view, chasing a pointer into a
    // GPU-resource object; now the sphere is built once here and the cull reads
    // only its own stream.
    Vec3     boundsCenter { 0.0f, 0.0f, 0.0f };
    Vec3     boundsSize   { 0.0f, 0.0f, 0.0f };
    uint32_t meshKey = 0;                  // batching ids -> the sort key's base
    uint32_t matKey  = 0;

    // Fallback material HANDLE for this draw (per-entity override, else the
    // mesh's own). Submesh ranges carry their own and fall back to this —
    // resolved per range at draw time, not here.
    MaterialHandle material;

    int      boneCount = 0;
    bool     hasBounds = false;            // false = never culled

    // ── 128 bytes, and the two things that got it there ─────────────────────
    // It was 144, spanning three cache lines for a struct the submit path reads
    // by RANDOM index (draws are sorted, so items are visited out of order).
    //
    // `const Material* mat` and `const Texture* tex` were removed: they were
    // WRITE-ONLY. Extraction resolved both handles and stored the pointers, and
    // nothing ever read them — ForwardPipeline::bindMaterial re-resolves through
    // ctx.materials/ctx.textures. So they cost 16 bytes per item plus two
    // registry lookups per item per frame, for nothing. That pair of lookups is
    // exactly what an earlier differential measured at ~0.17 ms per 20 000 items
    // and dismissed as "not worth touching" — it was not a cost worth paying at
    // all.
    //
    // The rest is ordering: pointers and the matrix first, then the extraction-
    // only block, then the small scalars packed together instead of each padding
    // out to 8. Adding a field in the middle undoes this; add to the cold block.
};

// ── CullStreams — the cull's working set ─────────────────────────────────────
//
// One entry per RenderItem, holding the ONLY things the cull reads. Two reasons it
// exists, and the second is the bigger one:
//
//   1. RenderItem is 144 bytes and the cull touched offsets 0..141 — all three
//      cache lines — to extract a bounding sphere and two ids. Reading 24 bytes of
//      stream instead beats that.
//   2. THE SPHERE DOES NOT DEPEND ON THE CAMERA, yet it was recomputed per view:
//      once for the camera frustum and again for the light's. Computing it at
//      extraction, where the matrix is already in registers, does it once.
//
// INTERLEAVED {x,y,z,r}, NOT FOUR PARALLEL ARRAYS — and that is a reversal, so the
// reasoning is worth keeping. The first version used separate x/y/z/r arrays on the
// grounds that a 4-wide test wants to load four centres contiguously. Measured, that
// premise does not pay: over 100 000 spheres the scalar plane test costs 1.47 ns per
// item while the cull's real in-engine cost is ~33 ns per item, so the arithmetic is
// ~4% of the phase and a 2.3x NEON win on it is ~0.4% of the render path. What DOES
// pay is stream count: one 16-byte sequential read instead of four independent ones
// measured 1.27x faster on the same data, because four arrays are four streams for
// the prefetcher and four TLB entries.
//
// So the layout follows the memory, not the instruction set. If SIMD is revisited,
// arm64's `vld4q_f32` deinterleaves this layout in one instruction (measured at
// 1.98x — still not worth it, but not blocked either).
//
// RADIUS CARRIES TWO SENTINELS, so the hot loop needs no side table:
//   r <  0        the item is not renderable (no mesh) — skip it entirely
//   r == infinity renderable but unbounded — never culled, by design, because an
//                 unbounded mesh is a missing-data problem and dropping it
//                 silently would look like a renderer bug
struct CullSphere {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float r = 0.0f;                        // plus the sentinels above
};

struct CullStreams {
    Span<CullSphere> sphere;   // world bounding sphere, one 16-byte read per item
    Span<uint64_t>   keyBase;  // sort key with the depth field left zero

    std::size_t size() const { return sphere.size(); }
    bool empty() const { return sphere.empty(); }
    // Every stream must be the same length as the item array it describes.
    bool consistent(std::size_t items) const {
        return sphere.size() == items && keyBase.size() == items;
    }
};

// The camera half of a view: everything visibility needs, nothing it doesn't.
// Deliberately separate from RenderTarget, which is where bgfx begins.
struct ViewCamera {
    Mat4  view;
    Mat4  proj;
    Vec4  camPos;                 // w = 1
    float frustum[6][4] = {};     // normalized planes, engine-provided
};

// One frame, fully extracted: no ECS, no registries, no game-state pointers.
struct RenderWorld {
    Span<RenderItem> items;       // ALL renderables — visibility culls
    CullStreams      cull;        // parallel to `items`; see CullStreams
    Span<LightItem>  lights;
    float            ambient = 0.0f;
};
