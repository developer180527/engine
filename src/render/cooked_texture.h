#pragma once
// ── cooked_texture — the engine's texture format id -> bgfx's ───────────────
// The cooker packs blocks + a full mip chain in exactly bgfx's expected layout,
// so an upload is a header read and one create; this is the vocabulary map that
// create needs. Renderer-internal: gpu.cpp is its only includer.
//
// ── WHAT USED TO BE HERE ────────────────────────────────────────────────────
// A `createCookedTexture(TextureAsset)` lived beside this map, with the unknown-
// format and unsupported-format refusals. Those refusals moved to
// gpu::textureFormatSupported (2026-09-05) and the function lost its last
// caller, but it stayed — and colour pipeline stage A then taught it a colour-
// space rule of its own, with a comment claiming it matched AssetService's. It
// did not: no legacy-cache warning, and no fallback on a GPU without an sRGB
// variant of the format, where it would have handed bgfx a flag the backend
// refuses. A second copy of the one rule the stage exists to centralise, one
// caller away from being live, so it was deleted rather than fixed. Uploads go
// through gpu::createTexture2D; colour space through cookedColourSpace
// (runtime/services/texture_colour.h).
#include <assetlib/texture_asset.h>
#include <bgfx/bgfx.h>

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
