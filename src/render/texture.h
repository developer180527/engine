#pragma once
#include "render/gpu.h"
#include <cstdint>

struct Texture {
    gpu::TextureHandle handle;   // opaque since G1
    uint16_t width  = 0;
    uint16_t height = 0;

    Texture() = default;
    Texture(gpu::TextureHandle h, uint16_t w, uint16_t ht)
        : handle(h), width(w), height(ht) {}

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& o) noexcept
        : handle(o.handle), width(o.width), height(o.height) {
        o.handle = {};
    }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            destroy();
            handle = o.handle; width = o.width; height = o.height;
            o.handle = {};
        }
        return *this;
    }
    ~Texture() { destroy(); }
    bool valid() const { return handle.valid(); }
private:
    void destroy() { gpu::destroy(handle); }
};
