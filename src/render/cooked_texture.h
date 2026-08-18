#pragma once
// ── cooked_texture — GPU upload of block-compressed cooked textures ─────────
// The runtime half of the texture pipeline: the cooker packed blocks + a full
// mip chain in exactly bgfx's expected layout, so upload is a header read and
// one createTexture2D — zero CPU decode, zero conversion. v1 RGBA8 assets
// (format 0, 1 mip) upload through the same path unchanged.
//
// THREE FAMILIES reach here now (BC on desktop, ASTC and ETC2 on mobile), which
// makes the capability check below load-bearing rather than decorative: a build
// cooked for the wrong target must be diagnosable HERE, not by staring at a
// texture that came out as noise.
#include <assetlib/texture_asset.h>
#include <bgfx/bgfx.h>
#include <cstdio>

inline bgfx::TextureFormat::Enum cookedTexBgfxFormat(uint32_t f) {
    switch (f) {
        case assetlib::kTexBC7:     return bgfx::TextureFormat::BC7;
        case assetlib::kTexBC5:     return bgfx::TextureFormat::BC5;
        case assetlib::kTexBC1:     return bgfx::TextureFormat::BC1;
        case assetlib::kTexBC3:     return bgfx::TextureFormat::BC3;
        case assetlib::kTexASTC4x4: return bgfx::TextureFormat::ASTC4x4;
        case assetlib::kTexASTC6x6: return bgfx::TextureFormat::ASTC6x6;
        case assetlib::kTexASTC8x8: return bgfx::TextureFormat::ASTC8x8;
        case assetlib::kTexETC2:    return bgfx::TextureFormat::ETC2;
        case assetlib::kTexETC2A:   return bgfx::TextureFormat::ETC2A;
        case assetlib::kTexEACRG11: return bgfx::TextureFormat::EACRG11;
        case assetlib::kTexRGBA8:   return bgfx::TextureFormat::RGBA8;
        default:                    return bgfx::TextureFormat::Count;
    }
}

inline bgfx::TextureHandle createCookedTexture(const assetlib::TextureAsset& t) {
    if (t.pixels.empty()) return BGFX_INVALID_HANDLE;

    const bgfx::TextureFormat::Enum fmt = cookedTexBgfxFormat(t.header.format);

    // ── Two refusals that used to be one silent fallback ────────────────────
    // This function previously defaulted UNKNOWN format ids to RGBA8, which
    // handed block-compressed bytes to the driver as raw pixels: garbage on a
    // good day, a read past the end of a too-small buffer on a bad one. With
    // three families in play an id this build does not know is a real
    // possibility — a project cooked by a newer engine, or a .cache carried
    // across a version bump.
    if (fmt == bgfx::TextureFormat::Count) {
        std::printf("[CookedTexture] unknown format id %u (%ux%u) — refusing "
                    "upload. Cooked by a newer engine?\n",
                    t.header.format, t.header.width, t.header.height);
        return BGFX_INVALID_HANDLE;
    }

    // And a format the GPU cannot sample. This is the one that catches a
    // mis-targeted build: BC blobs on a phone, or ASTC on a desktop AMD part.
    // Saying so once per texture with the format NAMED is the difference
    // between a five-minute fix and an afternoon of blaming the shader.
    if (!bgfx::isTextureValid(0, false, 1, fmt, 0)) {
        std::printf("[CookedTexture] %s is not supported by this GPU (%ux%u) — "
                    "this content was cooked for a different target. Re-cook "
                    "with COOK_TEX_TARGET=bc|astc|etc2.\n",
                    assetlib::texFormatName(t.header.format),
                    t.header.width, t.header.height);
        return BGFX_INVALID_HANDLE;
    }

    return bgfx::createTexture2D(
        (uint16_t)t.header.width, (uint16_t)t.header.height,
        t.header.mipCount > 1, 1, fmt, 0,
        bgfx::copy(t.pixels.data(), (uint32_t)t.pixels.size()));
}
