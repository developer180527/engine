#pragma once
// ── MaterialCooker — .material -> .cmat ─────────────────────────────────────
//
// ONE concern: orchestration. Parse the manifest, read the referenced shader's
// DECLARED INTERFACE, resolve values against it, write the container.
//
// It reads the .shader MANIFEST, not the cooked .cshader. The interface is
// authored, not derived — compiling only VERIFIES it (ShaderCooker does that
// once). Reading the source means a material has no cook-order dependency on
// its shader, which avoids needing a deferral mechanism the pipeline doesn't
// have. The cost is that a material can cook against a shader whose own cook
// failed; that surfaces at load, and the shader's failure is already loud.
#include <assetlib/cooker.h>

#include <string>
#include <vector>
#include <filesystem>

class MaterialCooker : public assetlib::ICooker {
public:
    // v2: the .cmat format gained MaterialTexture::cooked (material_asset.h
    // kVersion 3). The FORMAT version alone is not enough — loadMaterial
    // rejects a version it does not know, so every previously cooked .cmat
    // would become unreadable while the DDC happily served it as up to date.
    // Bumping the COOKER version is what invalidates those entries.
    static constexpr uint32_t kVersion = 2;

    std::vector<std::string> extensions() const override { return { ".material" }; }
    assetlib::CookResult     cook(const assetlib::CookContext& ctx) override;

    const char* id()      const override { return "material"; }
    uint32_t    version() const override { return kVersion; }

    // The referenced .shader's bytes are an input: its declared defaults land
    // in this material's uniform blocks, and its parameter list decides what
    // validates. Editing the shader must therefore re-cook every material that
    // instances it — hashing it in is what makes that happen, since the shader
    // is a separate registry asset that this cooker only reads.
    std::string settingsFingerprint(const assetlib::CookContext& ctx) const override;
    // The .shader manifest this material resolves its parameters against.
    std::vector<std::filesystem::path>
    declaredInputs(const assetlib::CookContext& ctx) const override;

    // Pure JSON in, a few hundred bytes out. The default estimate (source x 10)
    // would reserve megabytes of budget for a 1 KB file and needlessly
    // serialize cooks behind it.
    size_t estimatePeakBytes(const assetlib::CookContext&) const override {
        return (size_t)8 << 20;
    }
};
