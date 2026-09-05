#pragma once
#include <cstdint>

// ── gpu — the ONLY thing outside src/render/renderer that names a GPU API ────
//
// This header exists so that loading and uploading stop being the same act.
//
// Five files used to do both: asset_service.cpp, async_loader/upload.cpp,
// mesh_loader.cpp, gltf_importer.cpp and assimp_importer.cpp each parsed bytes
// and then called bgfx to turn them into handles, in the same function. That
// coupling is what makes three separate things impossible:
//
//   * a HEADLESS build (a dedicated server, a cook worker, a test) — it has no
//     device, and yet it must still parse a mesh;
//   * a SECOND backend — every one of those call sites would need porting, and
//     they are not in the renderer, so nobody porting the renderer would find
//     them;
//   * an EMBEDDING HOST — vCAD hands us bytes and owns its own device.
//
// So the rule this header enforces is one sentence:
//
//   ** LOADING PRODUCES BYTES. UPLOADING PRODUCES HANDLES. THEY ARE DIFFERENT
//      FUNCTIONS, AND ONLY THE SECOND ONE NEEDS A GPU. **
//
// See docs/rhi/evidence-coupling.md section 2.1 and docs/rhi/phases.md G1.
//
// ── What this is NOT ────────────────────────────────────────────────────────
// This is not the RHI. The RHI (docs/rhi/) is a bindless, GPU-driven API with
// queues, timelines and a render graph, and it replaces the whole renderer's
// relationship with the driver. THIS is a narrow seam over exactly the five
// operations the asset path performs, and it exists so that G1 can land and be
// worth having whether or not the RHI is ever built. Do not grow it toward the
// RHI; if you need a queue here, you are in the wrong file.
namespace gpu {

inline constexpr uint16_t kInvalidIdx = UINT16_MAX;

// Vertex and index buffers are separate types because they are separate pools
// with a SHARED index space underneath — a single Handle type would destroy the
// wrong resource without ever looking invalid, which is the class of bug that
// takes a week to find.
struct VertexBufferHandle {
    uint16_t idx = kInvalidIdx;
    bool valid() const { return idx != kInvalidIdx; }
};
struct IndexBufferHandle {
    uint16_t idx = kInvalidIdx;
    bool valid() const { return idx != kInvalidIdx; }
};
struct TextureHandle {
    uint16_t idx = kInvalidIdx;
    bool valid() const { return idx != kInvalidIdx; }
};
// A render target. The renderer creates these directly through the backend —
// nothing in this header creates one — but the TYPE lives here so that
// render/renderer.h can declare its targets without naming a graphics API,
// which is what lets runtime.h stop pulling one in transitively (G1c).
struct FrameBufferHandle {
    uint16_t idx = kInvalidIdx;
    bool valid() const { return idx != kInvalidIdx; }
};

// A draw-order bucket. Deliberately a plain integer alias and NOT an opaque
// handle: view ids are compared, incremented and used as array indices all over
// the renderer, and wrapping them would be ceremony without a defect to point
// at. It is here so the vocabulary is in one place.
//
// This is a bgfx concept and it does not survive the RHI — passes there declare
// their own ordering through the render graph (docs/rhi/design-axioms.md axiom
// 4). Do not build anything new on it.
using ViewId = uint16_t;

// What a target clears before drawing. The engine's own names, because
// RenderTarget's default lives in a header the runtime includes — and a
// default spelled BGFX_CLEAR_COLOR is a backend name in a struct that has no
// other reason to know one. The values match bgfx's today; the renderer maps
// them explicitly anyway (render/gpu_bgfx.h), so nothing depends on that.
enum ClearFlags : uint16_t {
    kClearNone    = 0,
    kClearColor   = 1u << 0,
    kClearDepth   = 1u << 1,
    kClearStencil = 1u << 2,
};

// The engine has exactly two vertex layouts, so this is an enum rather than a
// layout object. That is deliberate: passing a backend's layout descriptor
// through the asset path would put the backend back in the headers this file
// exists to clear, and "Standard or Skinned" is the vocabulary the engine
// actually thinks in.
enum class VertexFormat : uint8_t { Standard, Skinned };
enum class IndexFormat  : uint8_t { U16, U32 };

// An opaque staging allocation. Backed by the backend's own pool, which is why
// this is a pointer to an incomplete type rather than a span: the allocation
// must outlive the call and is consumed by whichever create() takes it.
//
// LIFETIME, stated exactly, because the imprecise version of this comment hid a
// leak for a day:
//
//   * A Blob that REACHES the backend is consumed — freed by the backend
//     whether or not the resource was created successfully.
//   * A Blob that a create*() REFUSES before reaching the backend is
//     **stranded for the life of the process.** The backend exposes no way to
//     release staging memory that no command ever consumed.
//   * There is no gpu::free, and never will be. Do not try to free one.
//
// The only refusals that can strand a blob are the two FORMAT checks in
// createTexture2D, and callers must therefore ask `textureFormatSupported()`
// BEFORE staging. That is not merely hygiene: on content cooked for the wrong
// target every texture in the scene takes the refusal path, so the leak is the
// whole texture set rather than one texture — and pre-checking also keeps the
// 64 MB decode-and-memcpy off a path whose answer is already known.
struct Blob;

// ── Staging: safe from ANY thread ───────────────────────────────────────────
// This is the half of the work that is expensive (the memcpy of a 64 MB
// texture) and the half that does not need a device thread. The async loader
// does exactly this on its worker and leaves only handle creation for main.
Blob* copy(const void* data, uint32_t bytes);
// Uninitialised staging, for callers that build their payload in place.
Blob* alloc(uint32_t bytes);
// Writable pointer into an alloc()'d blob. Never call this on a blob that has
// already been passed to a create*().
void* blobData(Blob* b);
// Size in bytes. Null-safe (returns 0), because the residency bookkeeping that
// wants it runs for meshes that may legitimately have no index buffer.
uint32_t blobSize(const Blob* b);

// ── Creation: MAIN THREAD ONLY ──────────────────────────────────────────────
// Each takes ownership of its blob. An invalid handle back means the pool is
// full or the format is unsupported; the caller must check, because the
// engine's resource pools are finite and have been exhausted by real scenes
// (BGFX_CONFIG_MAX_INDEX_BUFFERS = 4096 once dropped 92% of a scene).
VertexBufferHandle createVertexBuffer(Blob* data, VertexFormat fmt);
IndexBufferHandle  createIndexBuffer(Blob* data, IndexFormat fmt);

// Can this build upload `format` (an assetlib::kTex* id) at all? False for an
// id this build does not know — content cooked by a newer engine — and for one
// the GPU cannot sample, which is what catches a mis-targeted build: BC blobs
// on a phone, or ASTC on a desktop AMD part.
//
// SAFE FROM ANY THREAD, and that is the point: it is a pure capability query
// against caps that are fixed at device creation, so a loader worker can ask it
// before deciding to spend a 64 MB memcpy. Returns false with no device.
//
// Reports each unsupported format ONCE per process, with the format named.
// Per-texture reporting drowned the real message in a scene where every texture
// fails for the same reason — which is exactly when this fires.
bool textureFormatSupported(uint32_t format);

// `format` is an assetlib::kTex* id — the ENGINE's format vocabulary, not a
// backend's. Keeping it that way is what lets this signature survive a backend
// swap unchanged.
//
// **Call textureFormatSupported() before staging the payload.** This function
// re-checks as a backstop, but a refusal HERE strands the blob (see Blob above),
// so reaching it is a caller bug rather than a supported path.
TextureHandle createTexture2D(uint16_t width, uint16_t height, uint16_t mips,
                              uint32_t format, Blob* data);

// ── Destruction ─────────────────────────────────────────────────────────────
// Each nulls the handle it is given, so a double destroy is a no-op rather than
// a use-after-free of a recycled slot.
void destroy(VertexBufferHandle& h);
void destroy(IndexBufferHandle& h);
void destroy(TextureHandle& h);
// Frame buffers are CREATED by the renderer through the backend directly (this
// header has no createFrameBuffer), but they are destroyed here so that the
// device-down guard is the same one every other resource gets — a target
// outliving the device was one of the ways this renderer used to crash on
// teardown.
void destroy(FrameBufferHandle& h);

// True when a device exists and creation can succeed. False in a headless
// process — a dedicated server, a cook worker, most tests — where every
// create*() returns an invalid handle and every destroy() is a no-op, so the
// asset path runs to completion and simply produces no handles.
bool deviceAvailable();

// HOST-INTERNAL. Called by Renderer::init/shutdown and by nothing else — it is
// the one place that knows a device came up. Declared here rather than in a
// detail header because there is exactly one caller and hiding it would cost
// more than it explains.
void setDeviceAvailable(bool available);

} // namespace gpu
