#include "render/shader/shader_select.h"

#include <cstdio>

namespace rshader {

bool profileForRenderer(RendererKind kind, uint32_t& outProfile) {
    switch (kind) {
        case RendererKind::Metal:      outProfile = assetlib::kProfileMetal; return true;
        case RendererKind::Vulkan:     outProfile = assetlib::kProfileSpirv; return true;
        case RendererKind::Direct3D11: outProfile = assetlib::kProfileDx11;  return true;
        case RendererKind::Direct3D12: outProfile = assetlib::kProfileDx12;  return true;
        case RendererKind::OpenGL:     outProfile = assetlib::kProfileGlsl;  return true;
        default:                       return false;
    }
}

VariantChoice selectVariant(const assetlib::ShaderAsset& sh,
                            uint32_t featureMask, RendererKind kind) {
    VariantChoice c;

    uint32_t profile = 0;
    if (!profileForRenderer(kind, profile)) {
        c.error = "renderer has no cooked shader profile (headless or unsupported "
                  "backend)";
        return c;
    }

    c.variant = sh.find(featureMask, profile);
    if (c.variant) return c;

    // Say WHICH of the two things went wrong, because the fixes are completely
    // different: a missing profile means re-cook for this target, a missing
    // mask means the material asked for a feature the shader doesn't have.
    bool profileCooked = false, maskCooked = false;
    for (const auto& v : sh.variants) {
        if (v.profile == profile)         profileCooked = true;
        if (v.featureMask == featureMask) maskCooked    = true;
    }

    if (!profileCooked) {
        std::string have;
        for (const auto& v : sh.variants) {
            const char* n = assetlib::profileName(v.profile);
            if (have.find(n) == std::string::npos) {
                if (!have.empty()) have += ", ";
                have += n;
            }
        }
        c.error = "shader \"" + sh.name + "\" was not cooked for "
                + assetlib::profileName(profile) + " (cooked: "
                + (have.empty() ? "nothing" : have)
                + ") — this package was built for a different backend";
        return c;
    }
    if (!maskCooked) {
        c.error = "shader \"" + sh.name + "\" has no variant for feature mask 0x"
                + [&]{ char b[16]; std::snprintf(b, sizeof(b), "%x", featureMask);
                       return std::string(b); }();
        return c;
    }
    c.error = "shader \"" + sh.name + "\" has that profile and that feature mask, "
              "but not together";
    return c;
}

std::string programKey(const std::string& cookedPath, uint32_t featureMask,
                       uint32_t profile) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "#%x#%u", featureMask, profile);
    return "prog:" + cookedPath + suffix;
}

} // namespace rshader
