#pragma once

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <limits>
#include "core/handle.h"

struct Mesh {
    bgfx::VertexBufferHandle vbh        = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh        = BGFX_INVALID_HANDLE;
    uint32_t                 indexCount = 0;
    bool                     doubleSided = false;
    MaterialHandle           material;

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
    Mesh(bgfx::VertexBufferHandle v, bgfx::IndexBufferHandle i, uint32_t ic)
        : vbh(v), ibh(i), indexCount(ic) {}

    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& o) noexcept
        : vbh(o.vbh), ibh(o.ibh), indexCount(o.indexCount),
          doubleSided(o.doubleSided), material(o.material),
          boundsMin(o.boundsMin), boundsMax(o.boundsMax) {
        o.vbh = BGFX_INVALID_HANDLE;
        o.ibh = BGFX_INVALID_HANDLE;
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
            boundsMin   = o.boundsMin;
            boundsMax   = o.boundsMax;
            o.vbh       = BGFX_INVALID_HANDLE;
            o.ibh       = BGFX_INVALID_HANDLE;
            o.indexCount = 0;
        }
        return *this;
    }

    ~Mesh() { destroy(); }

    bool valid() const { return bgfx::isValid(vbh) && bgfx::isValid(ibh); }

private:
    void destroy() {
        if (bgfx::isValid(vbh)) { bgfx::destroy(vbh); vbh = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(ibh)) { bgfx::destroy(ibh); ibh = BGFX_INVALID_HANDLE; }
    }
};
