#pragma once
#include <vector>

#include "render/gpu.h"
#include <bx/math.h>
#include <limits>
#include <string>
#include "core/handle.h"

// Index range for one submesh within a shared VB+IB.
// Material is empty until MaterialCooker lands — uses mesh.material fallback.
struct SubmeshRange {
    uint32_t       indexOffset = 0;
    uint32_t       indexCount  = 0;
    MaterialHandle material;    // overrides mesh.material when valid
};

struct Mesh {
    // Opaque since G1 (docs/rhi/phases.md). These were bgfx handles, and that
    // put <bgfx/bgfx.h> into every file that so much as named a Mesh — which
    // is how five loaders ended up calling the driver directly.
    gpu::VertexBufferHandle vbh;
    gpu::IndexBufferHandle  ibh;
    uint32_t                 indexCount = 0;
    bool                     doubleSided = false;
    MaterialHandle           material;
    std::vector<SubmeshRange> submeshes; // empty = single draw using full ibh
    std::string              sourcePath; // absolute path; empty for procedural meshes

    bx::Vec3 boundsMin {
         std::numeric_limits<float>::infinity(),
         std::numeric_limits<float>::infinity(),
         std::numeric_limits<float>::infinity()
    };
    bx::Vec3 boundsMax {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };

    // Do the submesh ranges tile [0, indexCount) exactly — contiguous, ascending,
    // no gaps, no overlap, covering everything?
    //
    // The DEPTH-ONLY passes care: a shadow draw binds no material, so if the ranges
    // tile, drawing the whole index buffer once renders exactly the same geometry as
    // drawing every range, for one draw instead of N — and it lets the mesh instance.
    // MeshCooker builds ranges by appending sequentially and sums the same counts
    // into header.indexCount, so cooked meshes tile by construction; this checks it
    // rather than trusting it, because a mesh can also arrive through the source
    // importers and a future cooker change must not silently break shadows.
    //
    // O(submeshes) — averages under two on real content, and only the shadow pass
    // calls it, once per drawn caster.
    bool submeshesTile() const {
        uint32_t next = 0;
        for (const auto& s : submeshes) {
            if (s.indexOffset != next) return false;
            next += s.indexCount;
        }
        return next == indexCount;
    }

    bool hasBounds() const { return boundsMin.x <= boundsMax.x; }
    bx::Vec3 boundsCenter() const {
        return { (boundsMin.x+boundsMax.x)*0.5f,
                 (boundsMin.y+boundsMax.y)*0.5f,
                 (boundsMin.z+boundsMax.z)*0.5f };
    }
    bx::Vec3 boundsSize() const {
        return { boundsMax.x-boundsMin.x,
                 boundsMax.y-boundsMin.y,
                 boundsMax.z-boundsMin.z };
    }

    Mesh() = default;
    Mesh(gpu::VertexBufferHandle v, gpu::IndexBufferHandle i, uint32_t ic)
        : vbh(v), ibh(i), indexCount(ic) {}

    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& o) noexcept
        : vbh(o.vbh), ibh(o.ibh), indexCount(o.indexCount),
          doubleSided(o.doubleSided), material(o.material),
          sourcePath(std::move(o.sourcePath)),
          boundsMin(o.boundsMin), boundsMax(o.boundsMax),
          submeshes(std::move(o.submeshes)) {
        o.vbh        = {};
        o.ibh        = {};
        o.indexCount = 0;
    }

    Mesh& operator=(Mesh&& o) noexcept {
        if (this != &o) {
            destroy();
            vbh         = o.vbh;
            ibh         = o.ibh;
            indexCount  = o.indexCount;
            doubleSided = o.doubleSided;
            material    = o.material;
            sourcePath  = std::move(o.sourcePath);
            boundsMin   = o.boundsMin;
            boundsMax   = o.boundsMax;
            submeshes   = std::move(o.submeshes);
            o.vbh       = {};
            o.ibh       = {};
            o.indexCount = 0;
        }
        return *this;
    }

    ~Mesh() { destroy(); }

    bool valid() const { return vbh.valid() && ibh.valid(); }

private:
    void destroy() { gpu::destroy(vbh); gpu::destroy(ibh); }
};
