#include "assets/cookers/texture/texture_cooker.h"
#include "assets/cookers/texture/texture_encode.h"
#include <stb_image.h>
#include <cstdio>
#include <cstdlib>

std::string TextureCooker::settingsFingerprint(const assetlib::CookContext& ctx) const {
    // Mirrors the format choice in cook::encodeTexture exactly.
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq     = hqEnv && *hqEnv && hqEnv[0] != '0';
    const bool  normal = cook::looksLikeNormalMap(
        ctx.sourcePath.filename().string().c_str());
    return std::string("hq=") + (hq ? "1" : "0")
         + ";normal="         + (normal ? "1" : "0");
}

size_t TextureCooker::estimatePeakBytes(const assetlib::CookContext& ctx) const {
    int w = 0, h = 0, c = 0;
    if (stbi_info(ctx.sourcePath.string().c_str(), &w, &h, &c) && w > 0 && h > 0) {
        // Peak resident while cooking one texture: decoded RGBA8 (4 B/px) +
        // the BC7 RGBA32F working buffer (16 B/px) + the mip tail (~1.33x) +
        // encoder scratch. ~24 B/px is a safe ceiling (8K ≈ 1.6 GB).
        const size_t px = (size_t)w * (size_t)h;
        return px * 24 + ((size_t)16 << 20);
    }
    return assetlib::ICooker::estimatePeakBytes(ctx);   // header unreadable
}

assetlib::CookResult TextureCooker::cook(const assetlib::CookContext& ctx) {
    int w = 0, h = 0, ch = 0;
    uint8_t* px = stbi_load(ctx.sourcePath.string().c_str(), &w, &h, &ch, 4);
    if (!px) {
        return { .success=false,
                 .error=std::string("stbi_load failed: ") + stbi_failure_reason() };
    }

    // Block-compress + full mip chain (texture_encode.h): BC7 for color,
    // BC5 for normal maps (filename heuristic — standalone files carry no
    // usage info; mesh cookers pass the material slot explicitly). Raw
    // RGBA32 was 64MB per 4k texture with no mips — a bandwidth trap.
    const bool isNormal = cook::looksLikeNormalMap(
        ctx.sourcePath.filename().string().c_str());
    assetlib::TextureAsset asset;
    const bool ok = cook::encodeTexture(px, (uint32_t)w, (uint32_t)h,
                                        isNormal, asset);
    stbi_image_free(px);
    if (!ok)
        return { .success=false, .error="BC encode failed" };

    if (!assetlib::saveTexture(asset, ctx.outputPath))
        return { .success=false, .error="saveTexture failed" };

    std::printf("[TextureCooker] %s -> %dx%d %s, %u mips (%.1f MB -> %.1f MB)\n",
                ctx.sourcePath.filename().string().c_str(), w, h,
                isNormal ? "BC5" : "BC7", asset.header.mipCount,
                (double)w * h * 4 / (1024.0 * 1024.0),
                asset.pixels.size() / (1024.0 * 1024.0));
    return { .success=true };
}
