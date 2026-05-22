#include "cookers/texture_cooker.h"
#include <stb_image.h>
#include <cstdio>

assetlib::CookResult TextureCooker::cook(const assetlib::CookContext& ctx) {
    int w = 0, h = 0, ch = 0;
    uint8_t* px = stbi_load(ctx.sourcePath.string().c_str(), &w, &h, &ch, 4);
    if (!px) {
        return { .success=false,
                 .error=std::string("stbi_load failed: ") + stbi_failure_reason() };
    }

    assetlib::TextureAsset asset;
    asset.header.width    = static_cast<uint32_t>(w);
    asset.header.height   = static_cast<uint32_t>(h);
    asset.header.channels = 4;
    asset.pixels.assign(px, px + w * h * 4);
    stbi_image_free(px);

    if (!assetlib::saveTexture(asset, ctx.outputPath))
        return { .success=false, .error="saveTexture failed" };

    std::printf("[TextureCooker] %s → %dx%d RGBA8\n",
                ctx.sourcePath.filename().string().c_str(), w, h);
    return { .success=true };
}
