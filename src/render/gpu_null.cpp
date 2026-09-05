// ── gpu_null — the upload seam with no backend behind it at all ─────────────
//
// The SERVER BUILD's replacement for render/gpu.cpp. Same symbols, same header,
// no graphics API: this is the translation unit that lets `engine_runtime_server`
// link without bgfx (docs/rhi/phases.md G1c step B).
//
// ── Why a whole TU instead of a flag ────────────────────────────────────────
// gpu.cpp ALREADY behaves exactly like this when no device exists — staging
// returns null, every create returns an invalid handle, destroy is a no-op. The
// difference is not behaviour, it is the DEPENDENCY: gpu.cpp reaches those
// answers by asking bgfx and therefore links it. This file reaches the same
// answers by construction.
//
// That equivalence is the property worth protecting, and it is asserted rather
// than asserted-in-prose: tests/gpu_seam_test.cpp §1 runs against the real
// gpu.cpp with no device and expects precisely the results below. A server build
// running that section must produce the same output, which is what makes "the
// headless build and the headless path agree" checkable instead of hopeful.
//
// ── The pattern, one layer down ─────────────────────────────────────────────
// NullRenderer is this idea at the renderer level; this is it at the driver
// level. Unreal's FNullDynamicRHI sits exactly here — at the RHI, below the
// renderer — and is selected by `-nullrhi` (docs/rhi/headless.md §3.1). The
// difference is that Epic swaps theirs at RUNTIME and we swap this one at LINK
// time, because our goal is a binary with no graphics dependency rather than one
// binary that can run either way.
//
// ── DO NOT make this clever ─────────────────────────────────────────────────
// Every function here returns the "nothing happened" answer. If one of them ever
// needs to remember something, the server build has started rendering and this
// file is the wrong place to notice that.
#include "render/gpu.h"

namespace gpu {

// A server never has a device, and nothing can give it one: the setter exists
// because Renderer::init calls it, and the real Renderer is not in this build.
// It is accepted and ignored rather than removed, so the header stays identical
// across both builds — one header, two implementations, which is the whole
// reason the rest of the engine does not know which it is compiled against.
void setDeviceAvailable(bool) {}
bool deviceAvailable() { return false; }

// Staging is where the memcpy would happen. Returning null here is what keeps a
// server from spending a 64 MB copy on a texture it will never upload — the same
// short-circuit the format pre-check gives the real build.
Blob* copy(const void*, uint32_t) { return nullptr; }
Blob* alloc(uint32_t)             { return nullptr; }
void* blobData(Blob*)             { return nullptr; }
uint32_t blobSize(const Blob*)    { return 0; }

// Invalid handles, which every caller already checks — the asset path was
// written to tolerate exactly this when G1a made the loaders device-free, so
// nothing above needs a server-specific branch.
VertexBufferHandle createVertexBuffer(Blob*, VertexFormat) { return {}; }
IndexBufferHandle  createIndexBuffer(Blob*, IndexFormat)   { return {}; }

// FALSE for every format, including RGBA8. Not "unknown format" — there is no
// GPU to sample anything, so no format is supported, and a loader asking this
// before staging correctly decides to skip the work entirely.
bool textureFormatSupported(uint32_t) { return false; }

TextureHandle createTexture2D(uint16_t, uint16_t, uint16_t, uint32_t, Blob*) {
    return {};
}

// Null the handle, as the real implementation does, so double-destroy is a
// no-op on both builds rather than only on one.
void destroy(VertexBufferHandle& h) { h.idx = kInvalidIdx; }
void destroy(IndexBufferHandle& h)  { h.idx = kInvalidIdx; }
void destroy(TextureHandle& h)      { h.idx = kInvalidIdx; }
void destroy(FrameBufferHandle& h)  { h.idx = kInvalidIdx; }

} // namespace gpu
