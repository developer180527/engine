#pragma once
// ── OutputPass — linear HDR scene -> sRGB-encoded display image ──────────────
//
// Colour pipeline stage A (docs/plans/colour-pipeline.md). RENDERER-INTERNAL:
// it names bgfx, so it lives under render/renderer/ and renderer.h holds it
// through an opaque pointer, keeping the public header free of any backend type
// (scripts/check_gpu_seam.py).
//
// ── WHY THE RENDERER OWNS IT, NOT THE PIPELINE ──────────────────────────────
// A pipeline is a swappable statement about how surfaces LOOK. Turning linear
// light into what a display expects is not a look — it is the contract between
// the renderer and the screen — so every pipeline, including one a project
// supplies, gets it without reimplementing it. The pipeline renders into the HDR
// target it is handed and never learns an output pass exists.
//
// ── WHERE IT RUNS ───────────────────────────────────────────────────────────
// bgfx executes views in id order. The engine's scene views are 0..4 and
// RenderContext::allocView hands out 5 upward; the editor's ImGui starts at 200.
// Output views sit at 190..192 — after every engine view has filled its HDR
// target, before ImGui composites the result. That caps allocView below 190;
// the number is RenderContext::kFirstOutputView and allocView asserts it.
#include <bgfx/bgfx.h>

#include "render/render_context.h"

struct OutputPass {
    static constexpr bgfx::ViewId kFirstView = RenderContext::kFirstOutputView;

    bgfx::ProgramHandle       program   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle       sHdr      = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout        layout;
    // The scene target's format. RGBA16F where the backend can render to it,
    // RGBA8 otherwise — decided once at create() from the device caps, and
    // reported if it falls back, because an 8-bit "HDR" target clips at 1.0 and
    // bands in the darks.
    bgfx::TextureFormat::Enum hdrFormat = bgfx::TextureFormat::RGBA16F;

    // After bgfx::init. False only if the program could not be built, in which
    // case submit() is a no-op and the display targets stay at their clear.
    bool create();
    void destroy();

    // Encode `hdr` into `dst` (BGFX_INVALID_HANDLE = the backbuffer) at w x h, on
    // view `view`. One fullscreen triangle; overwrites every pixel, so the view
    // does not clear.
    void submit(bgfx::ViewId view, bgfx::TextureHandle hdr,
                bgfx::FrameBufferHandle dst, uint16_t w, uint16_t h);
};
