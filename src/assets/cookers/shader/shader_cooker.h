#pragma once
// ── ShaderCooker — .shader -> .cshader ──────────────────────────────────────
//
// ONE concern: orchestration. Parse the manifest, compile every variant,
// verify the declared interface against the bytecode, write the container.
// The four steps each live in their own TU; this file only sequences them.
//
// Shaders become cooked content like meshes and textures, which means they
// inherit the whole existing pipeline for free: DDC caching, the thermal
// governor, memory-budgeted admission, and .cache GC. That is the argument for
// doing this through ICooker rather than a bespoke build step — the machinery
// already exists and is proven.
//
// Which backend profiles get cooked is a BUILD-TARGET question, not a property
// of the asset, so it comes from the environment rather than the .shader file:
//
//   COOK_SHADER_PROFILES=metal,spirv    (default: everything this host can emit)
//
// It feeds settingsFingerprint, so cooking for a new target re-cooks rather
// than serving a cache entry that lacks the profile.
#include <assetlib/cooker.h>
#include <assetlib/shader_asset.h>

#include <string>
#include <vector>

class ShaderCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 1;

    std::vector<std::string> extensions() const override { return { ".shader" }; }
    assetlib::CookResult     cook(const assetlib::CookContext& ctx) override;

    const char* id()      const override { return "shader"; }
    uint32_t    version() const override { return kVersion; }

    // The profile set changes the output for identical source bytes, and so
    // does the shaderc binary itself — a bgfx bump changes the bytecode
    // format. Both must key the cache.
    std::string settingsFingerprint(const assetlib::CookContext& ctx) const override;
    // The .sc stage sources and everything they #include, transitively.
    std::vector<std::filesystem::path>
    declaredInputs(const assetlib::CookContext& ctx) const override;

    // Shader compiles are small next to a mesh import or an 8K BC7 encode, but
    // they are not free: a full variant matrix spawns one shaderc per
    // (variant × profile × stage), each with its own glslang/SPIRV-Cross
    // arena. Reported so the budget scheduler doesn't pack dozens at once.
    size_t estimatePeakBytes(const assetlib::CookContext& ctx) const override;

    // The profiles this cook will target, resolved from the environment and
    // filtered to what the host can actually emit. Exposed for testing and for
    // the cook log, which should say what it targeted rather than leaving a
    // missing-backend build to be discovered on the target machine.
    static std::vector<uint32_t> resolveProfiles(std::string* whyEmpty = nullptr);
};
