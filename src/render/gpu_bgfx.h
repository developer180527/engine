#pragma once
#include <bgfx/bgfx.h>

#include "render/gpu.h"

// ── gpu_bgfx — converting an opaque handle back into a bgfx one ──────────────
//
// RENDERER-INTERNAL. Only files under src/render/ may include this, and only
// because the current backend IS bgfx: the passes still call bgfx::submit
// directly, and they need the underlying handle to do it.
//
// This header is the deliberate escape hatch that keeps G1 honest. The point of
// gpu.h is that the ASSET PATH stops naming a graphics API — not that the
// renderer pretends it has no backend. Somebody has to call the driver; the
// whole change is about making it one place instead of six.
//
// HOW TO TELL IF THIS HEADER IS BEING ABUSED: it should only ever be included
// by src/render/. An include from src/assets/, src/runtime/ or a kit means the
// asset path is re-acquiring the coupling that G1 removed, and the include-guard
// test (tests/gpu_seam_test.cpp) fails the build for it.
//
// WHEN THE RHI LANDS this file is deleted rather than ported: the passes will
// take rhi:: handles and there will be nothing to convert.
namespace gpu {

inline bgfx::VertexBufferHandle toBgfx(VertexBufferHandle h) {
    return bgfx::VertexBufferHandle{h.idx};
}
inline bgfx::IndexBufferHandle toBgfx(IndexBufferHandle h) {
    return bgfx::IndexBufferHandle{h.idx};
}
inline bgfx::TextureHandle toBgfx(TextureHandle h) {
    return bgfx::TextureHandle{h.idx};
}

// The reverse, for resources bgfx creates directly: the renderer's own render
// targets and the white / flat-normal fallbacks, and test fixtures that build a
// buffer with bgfx::makeRef and then hand it to a Mesh.
inline bgfx::FrameBufferHandle toBgfx(FrameBufferHandle h) {
    return bgfx::FrameBufferHandle{h.idx};
}
inline FrameBufferHandle fromBgfx(bgfx::FrameBufferHandle h) {
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}
inline TextureHandle fromBgfx(bgfx::TextureHandle h) {
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}
inline VertexBufferHandle fromBgfx(bgfx::VertexBufferHandle h) {
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}
inline IndexBufferHandle fromBgfx(bgfx::IndexBufferHandle h) {
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}

// Clear flags, mapped one by one rather than cast. The values happen to agree
// with bgfx's today; writing the mapping out is what makes that a coincidence
// we do not depend on.
inline uint16_t toBgfxClear(uint16_t flags) {
    uint16_t out = BGFX_CLEAR_NONE;
    if (flags & kClearColor)   out |= BGFX_CLEAR_COLOR;
    if (flags & kClearDepth)   out |= BGFX_CLEAR_DEPTH;
    if (flags & kClearStencil) out |= BGFX_CLEAR_STENCIL;
    return out;
}

} // namespace gpu
