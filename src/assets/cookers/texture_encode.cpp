#include "assets/cookers/texture_encode.h"

#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/error.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

inline size_t bcMipBytes(uint32_t w, uint32_t h, uint32_t bytesPerBlock) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bytesPerBlock;
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

    // ── Format choice ────────────────────────────────────────────────────
    // BC7 is near-lossless but bimg's only BC7 encoder is nvtt's AVPCL, which
    // runs an EXHAUSTIVE per-block endpoint search and IGNORES Quality::Fastest
    // (minutes per 4K, pins every core — the "melting laptop"). So BC7 is a
    // FINAL-BAKE format, opt-in via COOK_TEX_HQ, not the iteration default.
    // Default path uses squish (which DOES honor Fastest, ~orders faster):
    //   normals -> BC5 (two-channel XY)
    //   color   -> BC1 when opaque (8:1), BC3 when the source has alpha (4:1)
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq     = hqEnv && *hqEnv && hqEnv[0] != '0';

    bool hasAlpha = false;
    if (!isNormalMap) {
        const size_t n = (size_t)w * h * 4;
        for (size_t i = 3; i < n; i += 4)
            if (rgba[i] != 255) { hasAlpha = true; break; }
    }

    uint32_t fmt; bimg::TextureFormat::Enum bimgFmt;
    if (isNormalMap)   { fmt = assetlib::kTexBC5; bimgFmt = bimg::TextureFormat::BC5; }
    else if (hq)       { fmt = assetlib::kTexBC7; bimgFmt = bimg::TextureFormat::BC7; }
    else if (hasAlpha) { fmt = assetlib::kTexBC3; bimgFmt = bimg::TextureFormat::BC3; }
    else               { fmt = assetlib::kTexBC1; bimgFmt = bimg::TextureFormat::BC1; }
    const uint32_t bpb = assetlib::bcBytesPerBlock(fmt);

    out.header.width    = w;
    out.header.height   = h;
    out.header.channels = 4;
    out.header.version  = 2;
    out.header.format   = fmt;
    out.pixels.clear();

    std::vector<uint8_t> mip(rgba, rgba + (size_t)w * h * 4);
    std::vector<float>   mipF;   // BC7 float scratch only
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t mw = w, mh = h, mips = 0;
    for (;;) {
        ++mips;
        const size_t off = out.pixels.size();
        out.pixels.resize(off + bcMipBytes(mw, mh, bpb));
        bx::Error err;
        if (bimgFmt == bimg::TextureFormat::BC7) {
            // bimg's Rgba8 entry point rejects BC7 — the nvtt BC7 path is only
            // reachable through RGBA32F (how texturec feeds it). Convert the
            // mip. Quality::Default: AVPCL is exhaustive regardless, so take the
            // best quality since this is the explicit final bake.
            mipF.resize((size_t)mw * mh * 4);
            for (size_t i = 0; i < mipF.size(); ++i)
                mipF[i] = mip[i] / 255.0f;
            bimg::imageEncodeFromRgba32f(&g_encAlloc, out.pixels.data() + off,
                                         mipF.data(), mw, mh, 1, bimgFmt,
                                         bimg::Quality::Default, &err);
        } else {
            // squish (BC1/BC3/BC5): Fastest = range-fit, real-time-ish.
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
    static const char* kName[] = {"RGBA8","BC7","BC5","BC1","BC3"};
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("[TexEncode] %ux%u %s %u mips in %.0f ms\n", w, h,
                fmt < 5 ? kName[fmt] : "?", mips, ms);
    return true;
}

} // namespace cook
