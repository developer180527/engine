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

#include <memory>
#include <unordered_map>

#include "render/display_settings.h"
#include "render/render_context.h"

struct CubeLut;

struct OutputPass {
    static constexpr bgfx::ViewId kFirstView = RenderContext::kFirstOutputView;

    bgfx::ProgramHandle       program   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle       sHdr      = BGFX_INVALID_HANDLE;
    // ── Stage B: exposure, tone map, grade ──────────────────────────────────
    bgfx::UniformHandle       uDisplay  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle       uLutMin   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle       uLutScale = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle       sLut      = BGFX_INVALID_HANDLE;
    // Bound whenever a view has no grade, so the shader's 3D sampler is never
    // left empty — Metal and Vulkan validation both object to an unbound slot.
    bgfx::TextureHandle       identityLut = BGFX_INVALID_HANDLE;
    // 3D RGBA16F textures, which every backend this engine builds for supports;
    // checked once at create(), and a grade is skipped (reported once) if not.
    bool                      lutSupported = false;

    // One texture per LUT in use. Keyed by the parsed LUT's address, and holding
    // a reference to it, so the key cannot be reused by a different LUT while
    // the entry lives. LutLibrary never evicts, so neither does this: the set is
    // bounded by the distinct .cube files a session references.
    struct LutTexture {
        std::shared_ptr<const CubeLut> lut;
        bgfx::TextureHandle            tex = BGFX_INVALID_HANDLE;
    };
    std::unordered_map<const CubeLut*, LutTexture> luts;
    bgfx::TextureHandle lutTexture(const std::shared_ptr<const CubeLut>& lut);
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

    // Transform `hdr` into `dst` (BGFX_INVALID_HANDLE = the backbuffer) at w x h,
    // on view `view`, as `display` says. One fullscreen triangle; overwrites
    // every pixel, so the view does not clear.
    void submit(bgfx::ViewId view, bgfx::TextureHandle hdr,
                bgfx::FrameBufferHandle dst, uint16_t w, uint16_t h,
                const DisplayTransform& display);
};
