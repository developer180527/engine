#include "assets/cookers/texture/texture_encode.h"

// Fast CPU block compression (third_party/bc7enc — Rich Geldreich's
// bc7enc_rdo encoders, MIT/public domain):
//   rgbcx  — BC1/BC3/BC5, ~two orders of magnitude faster than squish's
//            cluster fit at equal-or-better quality
//   bc7enc — BC7 in seconds instead of nvtt AVPCL's exhaustive minutes
// This file is the ONLY encoder in the engine; shipped runtimes read blocks,
// never encode.
#include <rgbcx.h>
#include <bc7enc.h>

// The mobile families come from bimg, which already vendors ARM's astc-encoder
// and an ETC2 RGB encoder — both submodules this tree carries for bgfx. That is
// why adding ASTC/ETC2 needed no new dependency; only the `bimg_encode` target,
// which was dropped earlier because its BC path (squish cluster-fit, ~6.5 s per
// 4K BC1) was far slower than rgbcx. BC stays on rgbcx; bimg is used ONLY for
// the two families it is the best available option for.
#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/error.h>

#include "assets/cookers/texture/texture_eac.h"
#include "assets/cookers/texture/texture_target.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cook {

namespace {

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// sRGB transfer via lookup tables — the mip filter touches every texel of a
// ~1.33x-of-source pixel chain, and three pow() calls per pixel were seconds
// of the 4K cook all by themselves. Decode: exact 256-entry table. Encode:
// 16K entries over [0,1] — worst-case quantization error ~0.2/255, below the
// codec's own noise floor.
struct SrgbTables {
    float   toLinear[256];
    uint8_t toSrgb[16384];
    SrgbTables() {
        for (int i = 0; i < 256; ++i)
            toLinear[i] = srgbToLinear(i / 255.0f);
        for (int i = 0; i < 16384; ++i)
            toSrgb[i] = (uint8_t)std::lround(
                linearToSrgb((i + 0.5f) / 16384.0f) * 255.0f);
    }
    inline uint8_t encode(float linear) const {
        int idx = (int)(linear * 16384.0f);
        return toSrgb[std::clamp(idx, 0, 16383)];
    }
};
const SrgbTables& srgbTables() {
    static const SrgbTables t;
    return t;
}

// One 2x2 box-filter step. Color averages in LINEAR space (averaging sRGB
// bytes directly darkens mips — the classic wrong-mips bug); normal maps
// average the decoded vectors and renormalize so mip normals keep unit
// length instead of flattening toward grey.
void downsample2x2(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh,
                   std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh,
                   bool isNormalMap) {
    const SrgbTables& lut = srgbTables();
    dst.resize((size_t)dw * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) {
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = std::min(x * 2,     sw - 1);
            const uint32_t x1 = std::min(x * 2 + 1, sw - 1);
            const uint32_t y0 = std::min(y * 2,     sh - 1);
            const uint32_t y1 = std::min(y * 2 + 1, sh - 1);
            const uint8_t* p[4] = {
                &src[((size_t)y0 * sw + x0) * 4], &src[((size_t)y0 * sw + x1) * 4],
                &src[((size_t)y1 * sw + x0) * 4], &src[((size_t)y1 * sw + x1) * 4],
            };
            uint8_t* o = &dst[((size_t)y * dw + x) * 4];

            if (isNormalMap) {
                float nx = 0, ny = 0, nz = 0, a = 0;
                for (auto* s : p) {
                    nx += s[0] / 255.0f * 2.0f - 1.0f;
                    ny += s[1] / 255.0f * 2.0f - 1.0f;
                    nz += s[2] / 255.0f * 2.0f - 1.0f;
                    a  += s[3];
                }
                const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                else             { nx = 0; ny = 0; nz = 1; }
                o[0] = (uint8_t)std::lround((nx * 0.5f + 0.5f) * 255.0f);
                o[1] = (uint8_t)std::lround((ny * 0.5f + 0.5f) * 255.0f);
                o[2] = (uint8_t)std::lround((nz * 0.5f + 0.5f) * 255.0f);
                o[3] = (uint8_t)std::lround(a / 4.0f);
            } else {
                for (int c = 0; c < 3; ++c) {
                    const float sum = lut.toLinear[p[0][c]] + lut.toLinear[p[1][c]]
                                    + lut.toLinear[p[2][c]] + lut.toLinear[p[3][c]];
                    o[c] = lut.encode(sum * 0.25f);
                }
                float a = 0;
                for (auto* s : p) a += s[3];
                o[3] = (uint8_t)std::lround(a / 4.0f);
            }
        }
    }
}

// ── Padding, and the bimg over-read it exists to avoid ──────────────────────
// bimg's ETC2 block loop indexes straight off the source pointer with no edge
// clamp: for a mip whose width is not a multiple of 4 it reads past the end of
// the row, and for the last row past the end of the IMAGE. Our mip chains run
// down to 1x1, so mips 2x2 and 1x1 hit it on every single texture. (Its own
// destination-size math has the matching hole — dstPitch is width*bpp/8, which
// is 0 bytes for a 1-wide ETC2 mip that really needs 8.)
//
// So nothing goes to bimg at a partial-block size. Every mip is edge-replicated
// up to a whole number of blocks first, which is also what the BC path already
// does per-block in extractBlock. Replication rather than zero-fill: black
// padding bleeds into the last block's endpoints and shows up as a dark fringe
// along the right and bottom edges of every atlas.
void padToBlockMultiple(const std::vector<uint8_t>& src, uint32_t w, uint32_t h,
                        uint32_t blockW, uint32_t blockH,
                        std::vector<uint8_t>& dst, uint32_t& pw, uint32_t& ph) {
    pw = ((w + blockW - 1) / blockW) * blockW;
    ph = ((h + blockH - 1) / blockH) * blockH;
    if (pw == w && ph == h) { dst = src; return; }
    dst.resize((size_t)pw * ph * 4);
    for (uint32_t y = 0; y < ph; ++y) {
        const uint32_t sy = std::min(y, h - 1);
        for (uint32_t x = 0; x < pw; ++x) {
            const uint32_t sx = std::min(x, w - 1);
            std::memcpy(&dst[((size_t)y * pw + x) * 4],
                        &src[((size_t)sy * w + sx) * 4], 4);
        }
    }
}

bx::AllocatorI* bimgAllocator() {
    static bx::DefaultAllocator alloc;
    return &alloc;
}

// One mip through bimg, for the families it owns (ASTC via astc-encoder, ETC2
// RGB via its own encoder). `out` is sized by the caller from texMipBytes.
bool bimgEncodeMip(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h,
                   bimg::TextureFormat::Enum fmt, bool isNormalMap,
                   uint8_t* out, size_t outBytes) {
    const bimg::ImageBlockInfo& bi = bimg::getBlockInfo(fmt);
    std::vector<uint8_t> padded;
    uint32_t pw = 0, ph = 0;
    padToBlockMultiple(rgba, w, h, bi.blockWidth, bi.blockHeight, padded, pw, ph);

    const size_t blocks = (size_t)(pw / bi.blockWidth) * (ph / bi.blockHeight);
    if (blocks * (bi.blockSize) != outBytes) return false;

    bx::Error err;
    // Quality::Default even for normal maps, deliberately. bimg's
    // NormalMapDefault tier switches astcenc into ASTC_ENC_NORMAL_RA, which
    // stores X in R and Y in ALPHA — better error metrics, and a different
    // shader contract from BC5's .rg. One swizzle for every target is worth more
    // than a few dB on one of them; a per-target sampling rule is exactly the
    // kind of hidden divergence that makes a mobile port feel like a fork.
    (void)isNormalMap;
    bimg::imageEncodeFromRgba8(bimgAllocator(), out, padded.data(), pw, ph, 1,
                               fmt, bimg::Quality::Default, &err);
    return err.isOk();
}

// Copy one 4x4 block out of the mip, clamping (edge-replicating) at the
// right/bottom borders so partial blocks encode the same texels the GPU
// will sample.
inline void extractBlock(const uint8_t* rgba, uint32_t w, uint32_t h,
                         uint32_t bx, uint32_t by, uint8_t out[64]) {
    for (uint32_t py = 0; py < 4; ++py) {
        const uint32_t sy = std::min(by * 4 + py, h - 1);
        for (uint32_t px = 0; px < 4; ++px) {
            const uint32_t sx = std::min(bx * 4 + px, w - 1);
            std::memcpy(out + (py * 4 + px) * 4,
                        rgba + ((size_t)sy * w + sx) * 4, 4);
        }
    }
}

// rgbcx BC1/BC3 quality level (0..18). 2 is the sweet spot for iteration:
// still above squish cluster-fit quality, tens of megapixels per second.
constexpr uint32_t kBc1Level = 2;

void encodersInitOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init();                  // BC1-5 tables
        bc7enc_compress_block_init();   // BC7 tables ("or you'll get artifacts")
    });
}

// ── One mip, whichever family ───────────────────────────────────────────────
// The BC path walks 4x4 blocks itself (rgbcx/bc7enc are per-block APIs); the
// mobile families hand a whole mip to bimg or to our EAC encoder. `outBytes`
// comes from assetlib::texMipBytes, which is the only place block geometry
// lives — a 6x6 ASTC mip is NOT ceil(w/4)*ceil(h/4) blocks, and computing it
// locally is how a mip chain starts reading the next level as pixel data.
bool encodeMip(uint32_t fmt, const std::vector<uint8_t>& mip,
               uint32_t mw, uint32_t mh, bool isNormalMap,
               const bc7enc_compress_block_params& bc7Params,
               uint8_t* dst, size_t outBytes) {
    using namespace assetlib;

    switch (fmt) {
        // ── ASTC ────────────────────────────────────────────────────────────
        case kTexASTC4x4:
        case kTexASTC6x6:
        case kTexASTC8x8: {
            // Normal maps carry X,Y only — Z is reconstructed in-shader, the
            // same contract BC5 has. Flattening B to a constant before the
            // encode stops ASTC spending bits on a channel nobody samples;
            // leaving the authored Z in costs real quality at 6x6.
            std::vector<uint8_t> src = mip;
            if (isNormalMap)
                for (size_t i = 0; i < src.size(); i += 4) {
                    src[i + 2] = 0;
                    src[i + 3] = 255;
                }
            const bimg::TextureFormat::Enum bf =
                fmt == kTexASTC4x4 ? bimg::TextureFormat::ASTC4x4
              : fmt == kTexASTC6x6 ? bimg::TextureFormat::ASTC6x6
                                   : bimg::TextureFormat::ASTC8x8;
            return bimgEncodeMip(src, mw, mh, bf, isNormalMap, dst, outBytes);
        }

        // ── ETC2 RGB ────────────────────────────────────────────────────────
        case kTexETC2:
            return bimgEncodeMip(mip, mw, mh, bimg::TextureFormat::ETC2,
                                 isNormalMap, dst, outBytes);

        // ── ETC2 RGBA: 8 bytes of EAC alpha then 8 of ETC2 RGB, per block ───
        // bimg has the RGB half and no alpha encoder at all, so the two are
        // produced separately and interleaved here. The ORDER is not a choice:
        // COMPRESSED_RGBA8_ETC2_EAC puts alpha first, and swapping the halves
        // yields a texture that decodes to noise rather than to anything a
        // reviewer would recognise as "the alpha is wrong".
        case kTexETC2A: {
            const uint32_t bx_ = (mw + 3) / 4, by_ = (mh + 3) / 4;
            std::vector<uint8_t> rgbBlocks((size_t)bx_ * by_ * 8);
            if (!bimgEncodeMip(mip, mw, mh, bimg::TextureFormat::ETC2,
                               isNormalMap, rgbBlocks.data(), rgbBlocks.size()))
                return false;
            uint8_t block[64], alpha[16];
            for (uint32_t byi = 0; byi < by_; ++byi)
                for (uint32_t bxi = 0; bxi < bx_; ++bxi) {
                    extractBlock(mip.data(), mw, mh, bxi, byi, block);
                    for (int i = 0; i < 16; ++i) alpha[i] = block[i * 4 + 3];
                    const size_t bi = (size_t)byi * bx_ + bxi;
                    cook::encodeEacAlphaBlock(alpha, dst + bi * 16);
                    std::memcpy(dst + bi * 16 + 8, &rgbBlocks[bi * 8], 8);
                }
            return true;
        }

        // ── EAC RG11: the ETC2 answer to BC5 ────────────────────────────────
        case kTexEACRG11: {
            const uint32_t bx_ = (mw + 3) / 4, by_ = (mh + 3) / 4;
            uint8_t block[64], ch[16];
            for (uint32_t byi = 0; byi < by_; ++byi)
                for (uint32_t bxi = 0; bxi < bx_; ++bxi) {
                    extractBlock(mip.data(), mw, mh, bxi, byi, block);
                    uint8_t* d = dst + ((size_t)byi * bx_ + bxi) * 16;
                    for (int i = 0; i < 16; ++i) ch[i] = block[i * 4 + 0];
                    cook::encodeEacR11Block(ch, d);         // X
                    for (int i = 0; i < 16; ++i) ch[i] = block[i * 4 + 1];
                    cook::encodeEacR11Block(ch, d + 8);     // Y
                }
            return true;
        }

        // ── BC, per-block through rgbcx/bc7enc ──────────────────────────────
        default: {
            const uint32_t bpb = texBlockDims(fmt).bytes;
            const uint32_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            uint8_t block[64];
            for (uint32_t byi = 0; byi < bh; ++byi)
                for (uint32_t bxi = 0; bxi < bw; ++bxi) {
                    extractBlock(mip.data(), mw, mh, bxi, byi, block);
                    uint8_t* d = dst + ((size_t)byi * bw + bxi) * bpb;
                    switch (fmt) {
                        case kTexBC1:
                            rgbcx::encode_bc1(kBc1Level, d, block,
                                              /*allow_3color=*/false,
                                              /*transparent_black=*/false);
                            break;
                        case kTexBC3: rgbcx::encode_bc3(kBc1Level, d, block); break;
                        case kTexBC5: rgbcx::encode_bc5(d, block, 0, 1, 4);   break;
                        case kTexBC7:
                            bc7enc_compress_block(d, block, &bc7Params);
                            break;
                        default: return false;   // unknown format id
                    }
                }
            return true;
        }
    }
}

} // namespace

bool looksLikeNormalMap(const char* filename) {
    if (!filename) return false;
    std::string s(filename);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s.find("_nor") != std::string::npos ||
           s.find("normal") != std::string::npos ||
           s.find("_nrm") != std::string::npos;
}

bool encodeTexture(const uint8_t* rgba, uint32_t w, uint32_t h,
                   bool isNormalMap, assetlib::TextureAsset& out) {
    if (!rgba || w == 0 || h == 0) return false;
    encodersInitOnce();

    // ── Format choice ────────────────────────────────────────────────────
    // Two inputs, both from the environment rather than the asset: the quality
    // TIER (COOK_TEX_HQ) and the TARGET FAMILY (COOK_TEX_TARGET — bc, astc or
    // etc2). texFormatFor owns the whole decision so that
    // TextureCooker::settingsFingerprint can name the format without running an
    // encode; a fingerprint that re-derives the rule separately is a fingerprint
    // that will eventually describe a different cook than the one it keys.
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq     = hqEnv && *hqEnv && hqEnv[0] != '0';

    std::string why;
    const TexTarget target = resolveTexTarget(&why);
    if (!why.empty()) std::printf("[TexEncode] WARNING: %s\n", why.c_str());

    bool hasAlpha = false;
    if (!isNormalMap) {
        const size_t n = (size_t)w * h * 4;
        for (size_t i = 3; i < n; i += 4)
            if (rgba[i] != 255) { hasAlpha = true; break; }
    }

    const uint32_t fmt = texFormatFor(target, isNormalMap, hasAlpha, hq);

    bc7enc_compress_block_params bc7Params;
    bc7enc_compress_block_params_init(&bc7Params);

    out.header.width    = w;
    out.header.height   = h;
    out.header.channels = 4;
    out.header.version  = 2;
    out.header.format   = fmt;
    out.pixels.clear();

    std::vector<uint8_t> mip(rgba, rgba + (size_t)w * h * 4);
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t mw = w, mh = h, mips = 0;
    for (;;) {
        ++mips;
        const size_t off      = out.pixels.size();
        const size_t mipBytes = assetlib::texMipBytes(fmt, mw, mh);
        out.pixels.resize(off + mipBytes);
        if (!encodeMip(fmt, mip, mw, mh, isNormalMap, bc7Params,
                       out.pixels.data() + off, mipBytes)) {
            std::printf("[TexEncode] ERROR: %ux%u mip failed in format %u\n",
                        mw, mh, fmt);
            return false;
        }

        if (mw == 1 && mh == 1) break;
        const uint32_t nw = std::max(1u, mw >> 1);
        const uint32_t nh = std::max(1u, mh >> 1);
        std::vector<uint8_t> next;
        downsample2x2(mip, mw, mh, next, nw, nh, isNormalMap);
        mip.swap(next);
        mw = nw; mh = nh;
    }
    out.header.mipCount = mips;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("[TexEncode] %ux%u %s (%s) %u mips in %.0f ms\n", w, h,
                assetlib::texFormatName(fmt), texTargetName(target), mips, ms);
    return true;
}

} // namespace cook
