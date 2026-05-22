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
};
