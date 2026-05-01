#pragma once

#include <bgfx/bgfx.h>

// Mesh asset.
//
// Owns the GPU buffers that hold a 3D model's geometry. Heavy: backing memory
// is on the GPU. Many entities can share one Mesh by holding MeshHandles to it
// in their MeshRenderer components.
//
// Mesh is move-only because it owns GPU resources; copying would mean we'd
// have two C++ objects both trying to bgfx::destroy() the same handle on
// shutdown (a bug). Move semantics let us hand a Mesh off to AssetRegistry's
// internal storage cleanly.
struct Mesh {
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;

    // Number of indices, useful for stats / debugging. Not strictly needed
    // for rendering since bgfx tracks this internally, but handy.
    uint32_t indexCount = 0;

    Mesh() = default;

    Mesh(bgfx::VertexBufferHandle v, bgfx::IndexBufferHandle i, uint32_t ic)
        : vbh(v), ibh(i), indexCount(ic) {}

    // Move-only: see comment above.
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& o) noexcept
        : vbh(o.vbh), ibh(o.ibh), indexCount(o.indexCount) {
        o.vbh = BGFX_INVALID_HANDLE;
        o.ibh = BGFX_INVALID_HANDLE;
        o.indexCount = 0;
    }

    Mesh& operator=(Mesh&& o) noexcept {
        if (this != &o) {
            destroy();
            vbh = o.vbh;
            ibh = o.ibh;
            indexCount = o.indexCount;
            o.vbh = BGFX_INVALID_HANDLE;
            o.ibh = BGFX_INVALID_HANDLE;
            o.indexCount = 0;
        }
        return *this;
    }

    ~Mesh() { destroy(); }

    bool valid() const {
        return bgfx::isValid(vbh) && bgfx::isValid(ibh);
    }

private:
    void destroy() {
        if (bgfx::isValid(vbh)) { bgfx::destroy(vbh); vbh = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(ibh)) { bgfx::destroy(ibh); ibh = BGFX_INVALID_HANDLE; }
    }
};
