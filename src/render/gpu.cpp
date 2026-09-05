// ── gpu — the bgfx implementation of the asset path's upload seam ───────────
// The ONE translation unit outside src/render/renderer/ that turns engine bytes
// into GPU handles. A second backend adds a sibling .cpp; nothing else moves.
#include "render/gpu.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>

#include <bgfx/bgfx.h>

#include "render/cooked_texture.h"   // cookedTexBgfxFormat + the two refusals
#include "render/skinned_vertex.h"
#include "render/vertex.h"

namespace gpu {
namespace {

// Whether a device exists. Atomic because staging happens on loader workers
// while the main thread may be tearing the device down at shutdown; the worker
// must see a definite answer rather than a torn one.
std::atomic<bool> g_device{false};

const bgfx::Memory* mem(Blob* b) { return reinterpret_cast<const bgfx::Memory*>(b); }
Blob* blob(const bgfx::Memory* m) {
    return reinterpret_cast<Blob*>(const_cast<bgfx::Memory*>(m));
}

// ── The two vertex layouts, in the one file allowed to name a backend ───────
// These used to be static methods on Vertex and SkinnedVertex, which forced
// <bgfx/bgfx.h> into every header that named either — and from there into the
// glTF and Assimp importers, which is half of how G1's five files got that way.
const bgfx::VertexLayout& standardLayout() {
    static const bgfx::VertexLayout l = [] {
        bgfx::VertexLayout v;
        v.begin()
            .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        return v;
    }();
    return l;
}

const bgfx::VertexLayout& skinnedLayout() {
    static const bgfx::VertexLayout l = [] {
        bgfx::VertexLayout v;
        v.begin()
            .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Indices,   4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Weight,    4, bgfx::AttribType::Float)
            .end();
        return v;
    }();
    return l;
}

// The struct and its descriptor now live in different files, so the agreement
// between them is no longer visible in one place. This is the half of it a
// compiler can check: a field added to either struct without a matching
// attribute here — or the reverse — changes the stride and fails the build,
// rather than reading garbage off the end of every vertex at draw time.
static_assert(sizeof(Vertex)        == 48, "Vertex layout in gpu.cpp is out of sync");
static_assert(sizeof(SkinnedVertex) == 68, "SkinnedVertex layout in gpu.cpp is out of sync");

} // namespace

void setDeviceAvailable(bool available) { g_device.store(available); }
bool deviceAvailable()                  { return g_device.load(); }

// ── Staging ─────────────────────────────────────────────────────────────────
// bgfx::copy is thread-safe (its pool allocator wraps malloc), which is the
// property the async loader is built on: the 64 MB memcpy of a texture happens
// on the worker and the main thread only creates a handle.
//
// The null-when-headless guard is not a formality. bgfx::copy dereferences an
// allocator that only exists after bgfx::init, so calling it in a process with
// no device is a crash, not an error — and "parse a mesh in a test" is a thing
// this engine legitimately does.
Blob* copy(const void* data, uint32_t bytes) {
    if (!g_device.load() || data == nullptr || bytes == 0) return nullptr;
    return blob(bgfx::copy(data, bytes));
}

Blob* alloc(uint32_t bytes) {
    if (!g_device.load() || bytes == 0) return nullptr;
    return blob(bgfx::alloc(bytes));
}

void* blobData(Blob* b) {
    return b ? const_cast<uint8_t*>(mem(b)->data) : nullptr;
}

uint32_t blobSize(const Blob* b) {
    return b ? mem(const_cast<Blob*>(b))->size : 0u;
}

// ── Creation ────────────────────────────────────────────────────────────────
VertexBufferHandle createVertexBuffer(Blob* data, VertexFormat fmt) {
    if (!g_device.load() || !data) return {};
    const bgfx::VertexLayout& layout = (fmt == VertexFormat::Skinned)
        ? skinnedLayout()
        : standardLayout();
    const bgfx::VertexBufferHandle h = bgfx::createVertexBuffer(mem(data), layout);
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}

IndexBufferHandle createIndexBuffer(Blob* data, IndexFormat fmt) {
    if (!g_device.load() || !data) return {};
    const bgfx::IndexBufferHandle h = (fmt == IndexFormat::U32)
        ? bgfx::createIndexBuffer(mem(data), BGFX_BUFFER_INDEX32)
        : bgfx::createIndexBuffer(mem(data));
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}

// Reported-once bookkeeping. A mis-targeted build fails on EVERY texture for
// the same reason, and printing per texture buries the one line that explains
// it under a thousand identical ones.
bool reportOnce(uint32_t format) {
    static std::mutex          mu;
    static std::set<uint32_t>  seen;
    std::lock_guard<std::mutex> lk(mu);
    return seen.insert(format).second;
}

bool textureFormatSupported(uint32_t format) {
    if (!g_device.load()) return false;

    const bgfx::TextureFormat::Enum fmt = cookedTexBgfxFormat(format);

    // Two refusals that used to be one silent fallback. Defaulting an unknown id
    // to RGBA8 hands block-compressed bytes to the driver as raw pixels: garbage
    // on a good day, a read past the end of a too-small buffer on a bad one.
    if (fmt == bgfx::TextureFormat::Count) {
        if (reportOnce(format))
            std::printf("[gpu] unknown texture format id %u — refusing upload. "
                        "Cooked by a newer engine?\n", format);
        return false;
    }
    // isTextureValid reads caps fixed at device creation, so this is safe to ask
    // from a loader worker — which is the whole point: the answer decides
    // whether the 64 MB decode-and-memcpy is worth doing at all.
    if (!bgfx::isTextureValid(0, false, 1, fmt, 0)) {
        if (reportOnce(format))
            std::printf("[gpu] %s is not supported by this GPU — this content "
                        "was cooked for a different target. Re-cook with "
                        "COOK_TEX_TARGET=bc|astc|etc2.\n",
                        assetlib::texFormatName(format));
        return false;
    }
    return true;
}

TextureHandle createTexture2D(uint16_t width, uint16_t height, uint16_t mips,
                              uint32_t format, Blob* data) {
    if (!g_device.load() || !data) return {};

    // BACKSTOP, not the check. Callers pre-check with textureFormatSupported()
    // before staging, because refusing here strands `data` for the life of the
    // process — the backend has no way to release staging memory no command
    // consumed. Reaching this branch with a live blob is a caller bug, and it
    // says so rather than failing quietly.
    if (!textureFormatSupported(format)) {
        std::printf("[gpu] BUG: createTexture2D refused format %u (%ux%u) with a "
                    "staged payload — %u bytes are now stranded for the life of "
                    "the process. Call textureFormatSupported() BEFORE staging "
                    "(render/gpu.h).\n", format, width, height, blobSize(data));
        return {};
    }

    const bgfx::TextureHandle h = bgfx::createTexture2D(
        width, height, mips > 1, 1, cookedTexBgfxFormat(format), 0, mem(data));
    return { bgfx::isValid(h) ? h.idx : kInvalidIdx };
}

// ── Destruction ─────────────────────────────────────────────────────────────
// Guarded on the device flag as well as on validity: a handle outliving
// shutdown is normal (registries are torn down after the device on some paths)
// and destroying into a dead bgfx is a crash. bgfx frees everything it owns at
// shutdown anyway, so skipping is correct rather than a leak.
void destroy(VertexBufferHandle& h) {
    if (h.valid() && g_device.load()) bgfx::destroy(bgfx::VertexBufferHandle{h.idx});
    h.idx = kInvalidIdx;
}
void destroy(IndexBufferHandle& h) {
    if (h.valid() && g_device.load()) bgfx::destroy(bgfx::IndexBufferHandle{h.idx});
    h.idx = kInvalidIdx;
}
void destroy(TextureHandle& h) {
    if (h.valid() && g_device.load()) bgfx::destroy(bgfx::TextureHandle{h.idx});
    h.idx = kInvalidIdx;
}
void destroy(FrameBufferHandle& h) {
    if (h.valid() && g_device.load()) bgfx::destroy(bgfx::FrameBufferHandle{h.idx});
    h.idx = kInvalidIdx;
}

} // namespace gpu
