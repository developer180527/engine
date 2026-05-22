#pragma once
#include <cstdint>
#include <vector>
#include <filesystem>

namespace assetlib {

struct TextureHeader {
    uint32_t magic    = 0x54455820; // 'TEX '
    uint32_t version  = 1;
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t channels = 4;          // always RGBA8 after cook
    uint8_t  _pad[12] = {};
};
static_assert(sizeof(TextureHeader) == 32, "TextureHeader size changed");

struct TextureAsset {
    TextureHeader        header;
    std::vector<uint8_t> pixels; // raw RGBA8, width*height*4 bytes
};

bool saveTexture(const TextureAsset& tex, const std::filesystem::path& outPath);
bool loadTexture(TextureAsset& out,       const std::filesystem::path& inPath);

} // namespace assetlib
