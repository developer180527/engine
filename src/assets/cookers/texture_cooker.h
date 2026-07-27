#pragma once
#include <assetlib/cooker.h>
#include <assetlib/texture_asset.h>
#include <vector>
#include <string>

class TextureCooker : public assetlib::ICooker {
public:
    // v2: DDC transition — this version now feeds the per-cooker cache key
    // (bumping it re-cooks textures and ONLY textures).
    // v3: rgbcx/bc7enc encoders replace squish/nvtt (different block bytes).
    static constexpr uint32_t kVersion = 3;

    std::vector<std::string> extensions() const override {
        return {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"};
    }
    assetlib::CookResult cook(const assetlib::CookContext& ctx) override;

    const char* id()      const override { return "texture"; }
    uint32_t    version() const override { return kVersion; }
    // Output varies with the COOK_TEX_HQ tier (BC7 vs fast squish) and the
    // filename normal-map heuristic (BC5 + linear mips) — both must key the
    // cache or an iteration-quality blob could satisfy a final-bake request.
    std::string settingsFingerprint(const assetlib::CookContext& ctx) const override;

    // Textures are the memory hogs the budget scheduler most needs to know
    // about: an 8K source decodes to 256 MB RGBA and ~1 GB in the BC7 float
    // path. Peek the header (no decode) for real dimensions.
    size_t estimatePeakBytes(const assetlib::CookContext& ctx) const override;
};
