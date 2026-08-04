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

// One drawable, resolved ONCE at extraction (handles -> pointers). POD + tight;
// meshKey/matKey are the batching sort keys.
struct RenderItem {
    Mat4            model;
    const Mesh*     mesh = nullptr;
    const Material* mat  = nullptr;   // resolved fallback material (see `material`)
    const Texture*  tex  = nullptr;
    uint32_t        meshKey = 0;
    uint32_t        matKey  = 0;
    // Fallback material HANDLE for this draw (per-entity override, else the
    // mesh's own material). Submesh ranges carry their own material and fall
    // back to this when unset — resolved per range at draw time, not here.
    MaterialHandle  material;

    // Non-null when this item has a SkinnedMesh component.
    // Points to SkinnedMesh::skinMatrices (kMaxBones * 16 floats).
    // Pipeline selects the skinned shader program when this is set.
    const float*    boneMatrices = nullptr;
    int             boneCount    = 0;

    // LOCAL-SPACE BOUNDS, COPIED AT EXTRACTION. Culling used to reach through
    // `mesh->boundsCenter()` for every item every frame: a pointer chase into
    // a GPU-resource object, per item, per view — and shadow cascades multiply
    // the view count. Copying 6 floats makes the cull loop a linear scan over
    // contiguous PODs, and (more importantly) lets visibility compile and be
    // tested without Mesh or bgfx at all.
    Vec3            boundsCenter { 0.0f, 0.0f, 0.0f };
    Vec3            boundsSize   { 0.0f, 0.0f, 0.0f };
    bool            hasBounds = false;   // false = never culled
};

// ── CullStreams — the cull's working set, SoA ────────────────────────────────
//
// Parallel arrays, one entry per RenderItem, holding the ONLY things the cull
// reads. Two reasons this exists, and the second is the bigger one:
//
//   1. RenderItem is 144 bytes and the cull touched offsets 0..141 — all three
//      cache lines — to extract a bounding sphere and two ids. Reading 24 bytes of
//      stream instead of 144 bytes of struct is the difference.
//   2. THE SPHERE DOES NOT DEPEND ON THE CAMERA, yet it was recomputed per view:
//      once for the camera frustum and again for the light's. Computing it at
//      extraction, where the matrix is already in registers, does it once.
//
// Separate arrays rather than an array of {x,y,z,r} structs, because the next step
// is a 4-wide test: four centres load contiguously from x[] and nothing is
// deinterleaved first.
//
// RADIUS CARRIES TWO SENTINELS, so the hot loop needs no side table:
//   r <  0        the item is not renderable (no mesh) — skip it entirely
//   r == infinity renderable but unbounded — never culled, by design, because an
//                 unbounded mesh is a missing-data problem and dropping it
//                 silently would look like a renderer bug
struct CullStreams {
    Span<float>    x, y, z;    // world bounding-sphere centre
    Span<float>    r;          // radius, plus the sentinels above
    Span<uint64_t> keyBase;    // sort key with the depth field left zero

    std::size_t size() const { return r.size(); }
    bool empty() const { return r.empty(); }
    // Every stream must be the same length as the item array it describes.
    bool consistent(std::size_t items) const {
        return r.size() == items && x.size() == items && y.size() == items
            && z.size() == items && keyBase.size() == items;
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
