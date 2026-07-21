#pragma once
#include <assetlib/cook_pipeline.h>
#include <assetlib/texture_asset.h>
#include <vector>
#include <string>

class TextureCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 1;

    std::vector<std::string> extensions() const override {
        return {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"};
    }
    assetlib::CookResult cook(const assetlib::CookContext& ctx) override;

    // Textures are the memory hogs the budget scheduler most needs to know
    // about: an 8K source decodes to 256 MB RGBA and ~1 GB in the BC7 float
    // path. Peek the header (no decode) for real dimensions.
    size_t estimatePeakBytes(const assetlib::CookContext& ctx) const override;
};
