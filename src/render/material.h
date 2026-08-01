#pragma once
#include "core/handle.h"

#include <string>
#include <vector>

// Material holds rendering parameters for a mesh.
// For now: base color texture + factor. More PBR params (roughness,
// metallic, normal map) added later when the shader supports them.
struct Material {
    TextureHandle baseColorTexture;            // invalid = no texture
    TextureHandle normalMapTexture;            // invalid = no normal map
    std::string   baseColorName;               // filename for inspector
    std::string   normalMapName;               // filename for inspector
    float         baseColorFactor[4] = {1,1,1,1};
    float         roughness          = 0.7f; // 0=mirror 1=fully rough
    float         metallic           = 0.0f; // 0=dielectric 1=metal
    bool          doubleSided        = false;

    bool hasTexture() const { return baseColorTexture.valid(); }

    // ── Data-driven path (a cooked .material) ───────────────────────────────
    // Populated by AssetService::loadMaterialAsset. When `dataDriven` is set,
    // the pipeline ignores every field above and uploads `blocks` verbatim: the
    // cooker already resolved names, checked types and filled defaults against
    // the shader's declared interface, so the runtime does no lookup and no
    // validation. That is the whole point — a misspelled parameter is a failed
    // COOK, not a silent no-op at 60 Hz.
    //
    // The fixed fields above remain for meshes whose materials come embedded in
    // cooked geometry (the importer path). They go away when every material is
    // an asset; see docs/process/roadmap.md Phase 5 step 4.
    struct UniformBlock {
        std::string        name;     // "u_params"
        std::vector<float> values;   // vec4-aligned, COMPLETE (never sparse)
    };
    struct TextureBind {
        std::string   uniform;       // "s_baseColor"
        uint32_t      stage = 0;
        TextureHandle texture;       // invalid -> bind `fallback`
        std::string   fallback;      // "white" | "flatNormal"
    };

    bool                      dataDriven = false;
    std::string               shaderName;      // resolved by ShaderLibrary
    uint32_t                  featureMask = 0;
    std::vector<UniformBlock> blocks;
    std::vector<TextureBind>  textureBinds;
};
