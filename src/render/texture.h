#pragma once
#include <bgfx/bgfx.h>
#include <cstdint>

struct Texture {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint16_t width  = 0;
    uint16_t height = 0;

    Texture() = default;
    Texture(bgfx::TextureHandle h, uint16_t w, uint16_t ht)
        : handle(h), width(w), height(ht) {}

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& o) noexcept
        : handle(o.handle), width(o.width), height(o.height) {
        o.handle = BGFX_INVALID_HANDLE;
    }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            destroy();
            handle = o.handle; width = o.width; height = o.height;
            o.handle = BGFX_INVALID_HANDLE;
        }
        return *this;
    }
    ~Texture() { destroy(); }
    bool valid() const { return bgfx::isValid(handle); }
private:
    void destroy() {
        if (bgfx::isValid(handle)) {
            bgfx::destroy(handle);
            handle = BGFX_INVALID_HANDLE;
        }
    }
};
