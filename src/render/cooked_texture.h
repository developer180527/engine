#pragma once
// ── cooked_texture — GPU upload of block-compressed cooked textures ─────────
// The runtime half of the texture pipeline: the cooker packed BC7/BC5
// blocks + a full mip chain in exactly bgfx's expected layout, so upload is
// a header read + one createTexture2D — zero CPU decode, zero conversion.
// v1 RGBA8 assets (format 0, 1 mip) upload through the same path unchanged.
#include <assetlib/texture_asset.h>
#include <bgfx/bgfx.h>

inline bgfx::TextureFormat::Enum cookedTexBgfxFormat(uint32_t f) {
    switch (f) {
        case assetlib::kTexBC7: return bgfx::TextureFormat::BC7;
        case assetlib::kTexBC5: return bgfx::TextureFormat::BC5;
        default:                return bgfx::TextureFormat::RGBA8;
    }
}

inline bgfx::TextureHandle createCookedTexture(const assetlib::TextureAsset& t) {
    if (t.pixels.empty()) return BGFX_INVALID_HANDLE;
    return bgfx::createTexture2D(
        (uint16_t)t.header.width, (uint16_t)t.header.height,
        t.header.mipCount > 1, 1,
        cookedTexBgfxFormat(t.header.format), 0,
        bgfx::copy(t.pixels.data(), (uint32_t)t.pixels.size()));
}
