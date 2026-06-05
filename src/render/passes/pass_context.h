#pragma once
#include <bgfx/bgfx.h>
// ── PassContext ──────────────────────────────────────────────────────────────
// SCAFFOLD — see docs/renderer-architecture.md.
//
// The per-frame "blackboard" handed to every pass. Carries the prepared data,
// engine-side handles, the output target, and HAND-OFF SLOTS where one pass
// publishes a resource for a later pass to read. That explicit hand-off is the
// deliberately-simple stand-in for a frame graph's resource edges — we add a
// real graph only when these grow dependencies we can't track by hand.
struct RenderWorld;     // render/render_world.h   (TODO: extraction output)
struct RenderContext;   // render/render_context.h
struct RenderTarget;    // render/render_pipeline.h

struct PassContext {
    // Inputs (borrowed; valid for this frame only)
    const RenderWorld*  world  = nullptr;  // culled/sorted items + packed lights
    RenderContext*      ctx    = nullptr;  // registries, fallback tex, view cursor
    const RenderTarget* target = nullptr;  // final color/depth destination
    float view[16];
    float proj[16];

    // ── Inter-pass hand-off ──────────────────────────────────────────────────
    // Shadows: ShadowPass publishes -> OpaquePass consumes
    bgfx::TextureHandle shadowMap = BGFX_INVALID_HANDLE;
    float shadowMatrix[16];
    float shadowParams[4];                 // x=enabled y=bias z=texel w=lightIndex

    // Scene buffers: Opaque/Transparency write -> Post/Resolve read
    bgfx::TextureHandle sceneColor = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle sceneDepth = BGFX_INVALID_HANDLE;
};
