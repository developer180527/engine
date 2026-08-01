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

inline size_t bcMipBytes(uint32_t w, uint32_t h, uint32_t bytesPerBlock) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bytesPerBlock;
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
    //   normals -> BC5 (two-channel XY, Z reconstructed in-shader)
    //   color   -> BC1 when opaque (8:1), BC3 when the source has alpha (4:1)
    //   COOK_TEX_HQ=1 -> BC7 for color (near-lossless 4:1, final bake) —
    //   with bc7enc it's seconds per 4K, not nvtt's exhaustive minutes, but
    //   BC1/BC3 iteration cooks remain ~20x faster still.
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq     = hqEnv && *hqEnv && hqEnv[0] != '0';

    bool hasAlpha = false;
    if (!isNormalMap) {
        const size_t n = (size_t)w * h * 4;
        for (size_t i = 3; i < n; i += 4)
            if (rgba[i] != 255) { hasAlpha = true; break; }
    }

    enum class Codec { BC1, BC3, BC5, BC7 };
    Codec codec; uint32_t fmt;
    if (isNormalMap)   { codec = Codec::BC5; fmt = assetlib::kTexBC5; }
    else if (hq)       { codec = Codec::BC7; fmt = assetlib::kTexBC7; }
    else if (hasAlpha) { codec = Codec::BC3; fmt = assetlib::kTexBC3; }
    else               { codec = Codec::BC1; fmt = assetlib::kTexBC1; }
    const uint32_t bpb = assetlib::bcBytesPerBlock(fmt);

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
        const size_t off = out.pixels.size();
        out.pixels.resize(off + bcMipBytes(mw, mh, bpb));
        uint8_t*       dst = out.pixels.data() + off;
        const uint32_t bw  = (mw + 3) / 4;
        const uint32_t bh  = (mh + 3) / 4;
        uint8_t block[64];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                extractBlock(mip.data(), mw, mh, bx, by, block);
                uint8_t* d = dst + ((size_t)by * bw + bx) * bpb;
                switch (codec) {
                    case Codec::BC1:
                        rgbcx::encode_bc1(kBc1Level, d, block,
                                          /*allow_3color=*/false,
                                          /*transparent_black=*/false);
                        break;
                    case Codec::BC3:
                        rgbcx::encode_bc3(kBc1Level, d, block);
                        break;
                    case Codec::BC5:
                        rgbcx::encode_bc5(d, block, 0, 1, 4);
                        break;
                    case Codec::BC7:
                        bc7enc_compress_block(d, block, &bc7Params);
                        break;
                }
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
    static const char* kName[] = {"RGBA8","BC7","BC5","BC1","BC3"};
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("[TexEncode] %ux%u %s %u mips in %.0f ms\n", w, h,
                fmt < 5 ? kName[fmt] : "?", mips, ms);
    return true;
}

} // namespace cook
