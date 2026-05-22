#include "assetlib/texture_asset.h"
#include <cstdio>
#include <cstring>

namespace assetlib {

bool saveTexture(const TextureAsset& tex, const std::filesystem::path& outPath) {
    FILE* f = std::fopen(outPath.string().c_str(), "wb");
    if (!f) return false;
    std::fwrite(&tex.header, sizeof(TextureHeader), 1, f);
    std::fwrite(tex.pixels.data(), 1, tex.pixels.size(), f);
    std::fclose(f);
    return true;
}

bool loadTexture(TextureAsset& out, const std::filesystem::path& inPath) {
    FILE* f = std::fopen(inPath.string().c_str(), "rb");
    if (!f) return false;
    TextureHeader h;
    if (std::fread(&h, sizeof(TextureHeader), 1, f) != 1 ||
        h.magic != 0x54455820) {
        std::fclose(f);
        return false;
    }
    out.header = h;
    const size_t pixelBytes = h.width * h.height * h.channels;
    out.pixels.resize(pixelBytes);
    std::fread(out.pixels.data(), 1, pixelBytes, f);
    std::fclose(f);
    return true;
}

} // namespace assetlib
