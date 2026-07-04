#pragma once
#include "core/math_types.h"
#include "core/handle.h"
#include "components/light.h"   // LightType
#include <bgfx/bgfx.h>
#include <cstddef>
#include <cstdint>

// Pipeline references GPU-side resources by pointer only; impls include these.
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
};

// Where a view renders. Lightweight: handles + dims + clear only.
struct RenderTarget {
    bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
    uint16_t w = 0, h = 0;
    Vec4     clearColor;
    uint16_t clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;
    float    clearDepth = 1.0f;
};

// Everything a pipeline needs to render ONE camera. Engine fills it; the
// pipeline consumes it and never touches the ECS.
struct RenderView {
    Mat4         view;
    Mat4         proj;
    Vec4         camPos;             // w = 1
    float        frustum[6][4] = {};
    RenderTarget target;
    bgfx::ViewId baseViewId = 0;     // engine-allocated; more via RenderContext
    Span<RenderItem> items;          // ALL renderables (unculled) — pipeline culls
    Span<LightItem>  lights;
    float ambient = 0.0f;            // grows into skybox / IBL
};
