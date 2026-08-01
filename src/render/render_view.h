#pragma once
// The GPU-FREE frame description (Span, LightItem, RenderItem, ViewCamera,
// RenderWorld) now lives in render/world/render_world.h so culling, sorting
// and light packing can be compiled and TESTED without bgfx. What remains here
// is the part that genuinely needs the graphics API: render targets and view
// ids. See docs/renderer-architecture.md §5.
#include "render/world/render_world.h"

#include <bgfx/bgfx.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

    // Views onto the GPU-free halves, so a pipeline can hand this straight to
    // rworld::buildVisibleSet / packLights without copying. RenderView stays
    // the one thing the engine fills; the machinery only ever sees the subset
    // it is allowed to touch.
    ViewCamera camera() const {
        ViewCamera c;
        c.view = view; c.proj = proj; c.camPos = camPos;
        std::memcpy(c.frustum, frustum, sizeof(frustum));
        return c;
    }
    RenderWorld world() const { return RenderWorld{ items, lights, ambient }; }
};
