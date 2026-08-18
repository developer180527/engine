#pragma once
#include <cstdint>
#include <vector>
#include <filesystem>

namespace assetlib {

// GPU-ready pixel formats. Cooked offline into hardware block-compressed
// data + a full mip chain, so the runtime does ZERO CPU work: read the
// header, hand the byte blocks straight to the GPU. Raw RGBA32 (v1) was a
// VRAM/bandwidth trap — 64MB per 4k texture, no mips, cold texture caches.
//
// THREE FAMILIES, because no single one runs everywhere:
//   BC   — desktop and Steam Deck. Every discrete/integrated PC GPU since
//          2010; NOT available on any iOS or Android device.
//   ASTC — the mobile primary. Every Metal-capable iPhone/iPad and every
//          modern Adreno/Mali/PowerVR. Not on AMD/NVIDIA desktop parts.
//   ETC2 — the compatibility floor: mandatory in GLES 3.0, so it covers the
//          old Android tail that predates ASTC. Lower quality than either of
//          the above at the same bitrate; it exists for reach, not looks.
//
// The FAMILY is a build-target decision (COOK_TEX_TARGET — see
// src/assets/cookers/texture/texture_target.h) and keys the DDC, so cooking
// for a phone cannot serve a desktop machine's cached BC blob. It is not a
// property of the source asset.
//
// Ids are APPEND-ONLY: a cooked .ctex on disk stores this number, so
// renumbering silently reinterprets every cached texture in every project.
enum TextureFormatId : uint32_t {
    kTexRGBA8    = 0,   // v1 legacy — raw, no mips
    kTexBC7      = 1,   // color HQ: near-lossless 4:1 — SLOW encode (final bake)
    kTexBC5      = 2,   // normal maps: two-channel XY, Z reconstructed in-shader
    kTexBC1      = 3,   // color, opaque: 8:1, fast squish (iteration default)
    kTexBC3      = 4,   // color, alpha: 4:1, fast squish (iteration default)
    // ── mobile ──
    kTexASTC4x4  = 5,   // color HQ / normals: 8.00 bpp
    kTexASTC6x6  = 6,   // color default:      3.56 bpp
    kTexASTC8x8  = 7,   // color, low tier:    2.00 bpp
    kTexETC2     = 8,   // color, opaque: RGB8, 4.00 bpp
    kTexETC2A    = 9,   // color, alpha:  RGBA8 (EAC alpha + ETC2 RGB), 8.00 bpp
    kTexEACRG11  = 10,  // normal maps: two-channel XY, the ETC2 answer to BC5
};

// ── Block geometry ──────────────────────────────────────────────────────────
// ASTC exists at block sizes other than 4x4, which the original mip-size math
// hard-coded. Everything that walks a mip chain must go through this: a 6x6
// ASTC mip is ceil(w/6)*ceil(h/6) blocks, not ceil(w/4)*ceil(h/4), and getting
// it wrong reads a neighbouring mip as pixel data.
struct TexBlockDims {
    uint32_t w;        // block width in texels
    uint32_t h;        // block height in texels
    uint32_t bytes;    // bytes per block
};

inline TexBlockDims texBlockDims(uint32_t fmt) {
    switch (fmt) {
        case kTexBC1:     return { 4, 4,  8 };
        case kTexBC3:
        case kTexBC5:
        case kTexBC7:     return { 4, 4, 16 };
        case kTexASTC4x4: return { 4, 4, 16 };   // every ASTC block is 128 bits
        case kTexASTC6x6: return { 6, 6, 16 };
        case kTexASTC8x8: return { 8, 8, 16 };
        case kTexETC2:    return { 4, 4,  8 };
        case kTexETC2A:   return { 4, 4, 16 };   // 8B EAC alpha + 8B ETC2 RGB
        case kTexEACRG11: return { 4, 4, 16 };   // 8B EAC R + 8B EAC G
        default:          return { 1, 1,  4 };   // kTexRGBA8 and anything unknown
    }
}

inline bool texIsBlockCompressed(uint32_t fmt) {
    return fmt != kTexRGBA8 && texBlockDims(fmt).w > 1;
}

// Bytes one mip of `fmt` occupies at these dimensions. Partial edge blocks
// count in full — that is what the hardware reads.
inline size_t texMipBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    const TexBlockDims b = texBlockDims(fmt);
    if (b.w == 1) return (size_t)w * h * b.bytes;
    return (size_t)((w + b.w - 1) / b.w) * ((h + b.h - 1) / b.h) * b.bytes;
}

// Total bytes of a full mip chain down to 1x1 — the payload size the loader
// should see. Cheap enough to check a cooked file against.
inline size_t texChainBytes(uint32_t fmt, uint32_t w, uint32_t h,
                            uint32_t mipCount) {
    // Straight count of mipCount levels, halving (floor 1) each time. The first
    // version carried a `(w > 1 || h > 1 || i == 0)` guard meant to stop a
    // too-large mipCount over-counting, and it dropped the FINAL 1x1 level of
    // every chain instead — so this function under-reported by one block on
    // every texture, and would have rejected every correctly cooked file it was
    // written to validate. A bad guard is worse than no guard: trust mipCount,
    // which the header carries precisely so this does not have to be inferred.
    size_t total = 0;
    for (uint32_t i = 0; i < mipCount; ++i) {
        total += texMipBytes(fmt, w, h);
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    return total;
}

// For logs and test failures. A cook that reports "BC7" while writing ASTC
// blocks costs an afternoon; the old version of this was a string array indexed
// by the format id, which silently became wrong the moment an id was appended.
inline const char* texFormatName(uint32_t fmt) {
    switch (fmt) {
        case kTexRGBA8:    return "RGBA8";
        case kTexBC7:      return "BC7";
        case kTexBC5:      return "BC5";
        case kTexBC1:      return "BC1";
        case kTexBC3:      return "BC3";
        case kTexASTC4x4:  return "ASTC4x4";
        case kTexASTC6x6:  return "ASTC6x6";
        case kTexASTC8x8:  return "ASTC8x8";
        case kTexETC2:     return "ETC2";
        case kTexETC2A:    return "ETC2A";
        case kTexEACRG11:  return "EACRG11";
        default:           return "?";
    }
}

// DEPRECATED — 4x4-only. Kept because the BC encode path predates block
// geometry; new code wants texBlockDims/texMipBytes, which are correct for
// ASTC's 6x6 and 8x8 as well.
inline uint32_t bcBytesPerBlock(uint32_t fmt) {
    return texBlockDims(fmt).bytes;
}

struct TextureHeader {
    uint32_t magic    = 0x54455820; // 'TEX '
    uint32_t version  = 2;          // v2: format + mipCount (v1 pads were 0)
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t channels = 4;
    uint32_t format   = kTexRGBA8;  // TextureFormatId
    uint32_t mipCount = 1;          // 0 (v1 pad) reads as 1
    uint8_t  _pad[4]  = {};
};
static_assert(sizeof(TextureHeader) == 32, "TextureHeader size changed");

struct TextureAsset {
    TextureHeader        header;
    // kTexRGBA8: raw width*height*4.
    // BC formats: every mip packed contiguous (mip0..mipN), each mip
    // ceil(w/4)*ceil(h/4)*bcBytesPerBlock(format) bytes at that mip's
    // dimensions (BC1 = 8, BC3/BC5/BC7 = 16) — exactly the layout bgfx
    // expects for a pre-mipped texture upload.
    std::vector<uint8_t> pixels;
};

bool saveTexture(const TextureAsset& tex, const std::filesystem::path& outPath);
bool loadTexture(TextureAsset& out,       const std::filesystem::path& inPath);

} // namespace assetlib
