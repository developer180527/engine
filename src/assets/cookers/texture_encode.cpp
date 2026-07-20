#include "assets/cookers/texture_encode.h"

#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/error.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace cook {

namespace {

bx::DefaultAllocator g_encAlloc;

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// One 2x2 box-filter step. Color averages in LINEAR space (averaging sRGB
// bytes directly darkens mips — the classic wrong-mips bug); normal maps
// average the decoded vectors and renormalize so mip normals keep unit
// length instead of flattening toward grey.
void downsample2x2(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh,
                   std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh,
                   bool isNormalMap) {
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
                    float sum = 0;
                    for (auto* s : p) sum += srgbToLinear(s[c] / 255.0f);
                    o[c] = (uint8_t)std::lround(
                        linearToSrgb(sum / 4.0f) * 255.0f);
                }
                float a = 0;
                for (auto* s : p) a += s[3];
                o[3] = (uint8_t)std::lround(a / 4.0f);
            }
        }
    }
}

inline size_t bcMipBytes(uint32_t w, uint32_t h) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;   // BC5/BC7: 16 B/block
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

    out.header.width    = w;
    out.header.height   = h;
    out.header.channels = 4;
    out.header.version  = 2;
    out.header.format   = isNormalMap ? assetlib::kTexBC5 : assetlib::kTexBC7;
    const auto bimgFmt  = isNormalMap ? bimg::TextureFormat::BC5
                                      : bimg::TextureFormat::BC7;
    out.pixels.clear();

    std::vector<uint8_t> mip(rgba, rgba + (size_t)w * h * 4);
    std::vector<float>   mipF;   // BC7 scratch — see below
    uint32_t mw = w, mh = h, mips = 0;
    for (;;) {
        ++mips;
        const size_t off = out.pixels.size();
        out.pixels.resize(off + bcMipBytes(mw, mh));
        bx::Error err;
        if (bimgFmt == bimg::TextureFormat::BC7) {
            // bimg's Rgba8 entry point rejects BC7 ("unable to convert") —
            // the nvtt BC7 compressor is only reachable through the RGBA32F
            // path (exactly how texturec feeds it). Convert the mip.
            //
            // Quality::Fastest, NOT Default: Default runs nvtt's exhaustive
            // partition/endpoint search — tens of millions of blocks per 4K
            // texture, every core pinned, minutes per asset. Fastest is
            // ~10-30x quicker with barely visible loss on albedo, which is
            // what shipping engines use for iteration cooks. Bump to Highest
            // only for a final master bake.
            mipF.resize((size_t)mw * mh * 4);
            for (size_t i = 0; i < mipF.size(); ++i)
                mipF[i] = mip[i] / 255.0f;
            bimg::imageEncodeFromRgba32f(&g_encAlloc, out.pixels.data() + off,
                                         mipF.data(), mw, mh, 1, bimgFmt,
                                         bimg::Quality::Fastest, &err);
        } else {
            bimg::imageEncodeFromRgba8(&g_encAlloc, out.pixels.data() + off,
                                       mip.data(), mw, mh, 1, bimgFmt,
                                       bimg::Quality::Fastest, &err);
        }
        if (!err.isOk()) return false;

        if (mw == 1 && mh == 1) break;
        const uint32_t nw = std::max(1u, mw >> 1);
        const uint32_t nh = std::max(1u, mh >> 1);
        std::vector<uint8_t> next;
        downsample2x2(mip, mw, mh, next, nw, nh, isNormalMap);
        mip.swap(next);
        mw = nw; mh = nh;
    }
    out.header.mipCount = mips;
    return true;
}

} // namespace cook
